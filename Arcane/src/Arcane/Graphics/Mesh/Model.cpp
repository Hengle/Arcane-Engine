#include "arcpch.h"
#include "Model.h"

#include <Arcane/Graphics/Shader.h>
#include <Arcane/Util/Loaders/AssetManager.h>
#include <Arcane/Animation/AnimationData.h>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

namespace Arcane
{
	Model::Model() : m_BoneCount(0)
	{
		m_Meshes.resize(0);
	}

	Model::Model(const Mesh &mesh) : m_BoneCount(0)
	{
		m_Meshes.push_back(mesh);
	}

	Model::Model(const std::vector<Mesh> &meshes) : m_BoneCount(0)
	{
		m_Meshes = meshes;
	}

	void Model::Draw(Shader *shader, RenderPassType pass) const
	{
		for (unsigned int i = 0; i < m_Meshes.size(); ++i) {
			// Avoid binding material information when it isn't needed
			if (pass == MaterialRequired) {
				m_Meshes[i].m_Material.BindMaterialInformation(shader);
			}
			m_Meshes[i].Draw();
		}
	}

	void Model::LoadModel(const std::string &path)
	{
		Assimp::Importer import;
		const aiScene *scene = import.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);

		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
		{
			ARC_LOG_ERROR("failed to load model - {0}", import.GetErrorString());
			return;
		}

		m_Directory = path.substr(0, path.find_last_of('/'));
		m_Name = path.substr(path.find_last_of("/\\") + 1);

		ProcessNode(scene->mRootNode, scene);
	}

	void Model::GenerateGpuData()
	{
		for (int i = 0; i < m_Meshes.size(); i++)
		{
			m_Meshes[i].GenerateGpuData();
		}
	}

	void Model::ProcessNode(aiNode *node, const aiScene *scene)
	{
		// Process all of the node's meshes (if any)
		for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
			// Each node has an array of mesh indices, use these indices to get the meshes from the scene
			aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
			ProcessMesh(mesh, scene);
		}
		// Process all of the node's children
		for (unsigned int i = 0; i < node->mNumChildren; ++i) {
			ProcessNode(node->mChildren[i], scene);
		}
	}

	void Model::ProcessMesh(aiMesh *mesh, const aiScene *scene)
	{
		std::vector<glm::vec3> positions;
		std::vector<glm::vec2> uvs;
		std::vector<glm::vec3> normals;
		std::vector<glm::vec3> tangents;
		std::vector<glm::vec3> bitangents;
		std::vector<unsigned int> indices;
		std::vector<VertexBoneData> boneWeights;

		positions.reserve(mesh->mNumVertices);
		uvs.reserve(mesh->mNumVertices);
		normals.reserve(mesh->mNumVertices);
		tangents.reserve(mesh->mNumVertices);
		bitangents.reserve(mesh->mNumVertices);
		if (mesh->mNumBones > 0)
			boneWeights.resize(mesh->mNumVertices);
		indices.reserve(mesh->mNumFaces * 3);

		// Process vertices
		for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
		{
			glm::vec2 uvCoord;
			// Texture Coordinates (check if there is texture coordinates)
			if (mesh->mTextureCoords[0])
			{
				// A vertex can contain up to 8 different texture coordinates. We are just going to use one set of TexCoords per vertex so grab the first one
				uvCoord.x = mesh->mTextureCoords[0][i].x;
				uvCoord.y = mesh->mTextureCoords[0][i].y;
			}
			else
			{
				uvCoord.x = 0.0f;
				uvCoord.y = 0.0f;
			}

			positions.push_back(glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z));
			uvs.push_back(glm::vec2(uvCoord.x, uvCoord.y));
			normals.push_back(glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z));
			tangents.push_back(glm::vec3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z));
			bitangents.push_back(glm::vec3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z));

			if (mesh->mNumBones > 0)
			{
				memset(boneWeights[i].BoneIDs, -1, sizeof(int) * MaxBonesPerVertex);
				memset(boneWeights[i].Weights, 0, sizeof(float) * MaxBonesPerVertex);
			}
		}

		// Save some animation related info
		m_GlobalInverseTransform = glm::inverse(ConvertAssimpMatrixToGLM(scene->mRootNode->mTransformation));

		// Process bones
		for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; boneIndex++)
		{
			int boneID = -1;
			aiBone *bone = mesh->mBones[boneIndex];

			// Get the bone name and if it doesn't exist let's add it to the bone array along with its matrix
			std::string boneName = bone->mName.C_Str();
			auto iter = m_BoneDataMap.find(boneName);
			if (iter == m_BoneDataMap.end())
			{
				BoneData newBoneData;
				newBoneData.boneID = m_BoneCount++;
				newBoneData.inverseBindPose = ConvertAssimpMatrixToGLM(bone->mOffsetMatrix);
				m_BoneDataMap[boneName] = newBoneData;
				boneID = newBoneData.boneID;
			}
			else
			{
				boneID = iter->second.boneID;
			}
			ARC_ASSERT(boneID != -1, "Bone not found or created..");

			// Now let's go through every vertex this bone affects and attempt to add the weight and index of the bone to that vertex
			aiVertexWeight *weights = bone->mWeights;
			int numWeights = bone->mNumWeights;
			for (int weightIndex = 0; weightIndex < numWeights; weightIndex++)
			{
				int vertexID = weights[weightIndex].mVertexId;
				float currentWeight = weights[weightIndex].mWeight;
				ARC_ASSERT(vertexID < (int)mesh->mNumVertices, "Bone data is trying to access an vertex that doesn't exist");

				// Let's attempt to add our bone weight and bone ID to the vertex data. It might be full since we limit how many bones can influence a single vertex
				bool foundSlot = false;
				for (int i = 0; i < MaxBonesPerVertex; i++)
				{
					// Check if a slot is empty, if so mark it as found, and fill the slot
					if (boneWeights[vertexID].BoneIDs[i] == -1)
					{
						boneWeights[vertexID].BoneIDs[i] = boneID;
						boneWeights[vertexID].Weights[i] = currentWeight;
						foundSlot = true;
						break;
					}
				}
				if (!foundSlot)
				{
					// Since we haven't found an open slot left, let's iterate over our slots and keep track of the lowest weight. This can be useful since all slots are full since we can replace a bone weight for the vertex
					// if another bone exists that has more influence. This is just working around bone vertex limitations, in such a way that hopefully reduces the quality loss when doing skeletal animation
					float lowestWeight = 1.0f; // Maximum weight a bone can have on a vert
					int smallestWeightIndex = -1;
					for (int i = 0; i < MaxBonesPerVertex; i++)
					{
						if (boneWeights[vertexID].Weights[i] < lowestWeight)
						{
							lowestWeight = boneWeights[vertexID].Weights[i];
							smallestWeightIndex = i;
						}
					}

					// Now let's check if we should replace
					if (currentWeight > lowestWeight && smallestWeightIndex != -1)
					{
						ARC_LOG_WARN("Hit Bone Vertex Capacity {0} on Vertex id:{1} - Replacing bone:{2} on the vert because it's influence:{3} is less than the bone:{4} we're trying to add's influence:{5}",
							MaxBonesPerVertex, vertexID, boneWeights[vertexID].BoneIDs[smallestWeightIndex], boneWeights[vertexID].Weights[smallestWeightIndex], boneID, currentWeight);

						boneWeights[vertexID].BoneIDs[smallestWeightIndex] = boneID;
						boneWeights[vertexID].Weights[smallestWeightIndex] = currentWeight;
					}
					else
					{
						ARC_LOG_WARN("Hit Bone Vertex Capacity {0} on Vertex id:{1} - Not adding bone:{2}'s influence amount:{3} because it is the least significant", MaxBonesPerVertex, vertexID, boneID, currentWeight);
					}
				}
			}
		}

		// Process Indices
		// Loop through every face (triangle thanks to aiProcess_Triangulate) and stores its indices in our meshes indices. This will ensure they are in the right order.
		for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
		{
			aiFace face = mesh->mFaces[i];
			for (unsigned int j = 0; j < face.mNumIndices; ++j)
			{
				indices.push_back(face.mIndices[j]);
			}
		}

		Mesh newMesh(std::move(positions), std::move(uvs), std::move(normals), std::move(tangents), std::move(bitangents), std::move(boneWeights), std::move(indices));
		newMesh.LoadData();

		// Process Materials (textures in this case)
		if (mesh->mMaterialIndex >= 0)
		{
			aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];

			// Attempt to load the materials if they can be found. However PBR materials will need to be manually configured since Assimp doesn't support them
			// Only colour data for the renderer is considered sRGB, all other type of non-colour texture data shouldn't be corrected by the hardware
			newMesh.m_Material.SetAlbedoMap(LoadMaterialTexture(material, aiTextureType_DIFFUSE, true));
			newMesh.m_Material.SetNormalMap(LoadMaterialTexture(material, aiTextureType_NORMALS, false));
			newMesh.m_Material.SetAmbientOcclusionMap(LoadMaterialTexture(material, aiTextureType_AMBIENT, false));
			newMesh.m_Material.SetDisplacementMap(LoadMaterialTexture(material, aiTextureType_DISPLACEMENT, false));
		}

		m_Meshes.emplace_back(newMesh);
	}

	Texture* Model::LoadMaterialTexture(aiMaterial *mat, aiTextureType type, bool isSRGB)
	{
		// Log material constraints are being violated (1 texture per type for the standard shader)
		if (mat->GetTextureCount(type) > 1)
			ARC_LOG_WARN("Mesh's default material contains more than 1 texture for the same type, which isn't currently supported by the standard shaders");

		// Load the texture of a certain type, assuming there is one
		if (mat->GetTextureCount(type) > 0)
		{
			aiString str;
			mat->GetTexture(type, 0, &str); // Grab only the first texture (standard shader only supports one texture of each type, it doesn't know how you want to do special blending)

			// Assumption made: material stuff is located in the same directory as the model object
			std::string fileToSearch = (m_Directory + "/" + std::string(str.C_Str())).c_str();

			TextureSettings textureSettings;
			textureSettings.IsSRGB = isSRGB;
			return AssetManager::GetInstance().Load2DTextureAsync(fileToSearch, &textureSettings);
		}

		return nullptr;
	}
}
