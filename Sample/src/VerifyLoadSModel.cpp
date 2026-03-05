#include "VerifyLoadSModel.h"

#include "assets/AssetManager.h"
#include "assets/MaterialAsset.h"
#include "assets/MeshAsset.h"
#include "assets/ModelAsset.h"
#include "assets/TextureAsset.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <functional>
#include <iostream>

namespace Sample
{
    Engine::ModelHandle VerifyLoadSModel(Engine::AssetManager &assets, const char *modelPath)
    {
        if (!modelPath)
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cerr << "[SMODEL] VerifyLoadSModel: modelPath is null\n";
#endif
            return {};
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "\n[SMODEL] Loading: " << modelPath << "\n";
#endif

        Engine::ModelHandle modelHandle = assets.loadModel(modelPath);
        if (!modelHandle.isValid())
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cerr << "[SMODEL] loadModel failed: " << modelPath << "\n";
#endif
            return {};
        }

        Engine::ModelAsset *model = assets.getModel(modelHandle);
        if (!model)
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cerr << "[SMODEL] getModel returned nullptr\n";
#endif
            return {};
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "[SMODEL] OK primitives=" << model->primitives.size() << "\n";
#endif

        if (model->primitives.empty())
        {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cerr << "[SMODEL] Model has 0 primitives (unexpected)\n";
#endif
            return modelHandle;
        }

        // Validate primitives -> mesh + material resolves
        for (size_t i = 0; i < model->primitives.size(); ++i)
        {
            const Engine::ModelPrimitive &prim = model->primitives[i];

            Engine::MeshAsset *mesh = assets.getMesh(prim.mesh);
            if (!mesh)
            {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cerr << "[SMODEL] Primitive " << i << ": mesh resolve failed\n";
#endif
                continue;
            }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cout << "  Prim[" << i << "] Mesh OK"
                      << " indices=" << prim.indexCount
                      << " vb=" << (void *)mesh->getVertexBuffer()
                      << " ib=" << (void *)mesh->getIndexBuffer()
                      << "\n";
#endif

            Engine::MaterialAsset *mat = assets.getMaterial(prim.material);
            if (!mat)
            {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "           Material: (missing)\n";
#endif
                continue;
            }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cout << "           Material OK"
                      << " baseColor=("
                      << mat->baseColorFactor[0] << ", "
                      << mat->baseColorFactor[1] << ", "
                      << mat->baseColorFactor[2] << ", "
                      << mat->baseColorFactor[3] << ")\n";
#endif

            // Optional: verify textures resolve
            if (mat->baseColorTexture.isValid())
            {
                Engine::TextureAsset *tex = assets.getTexture(mat->baseColorTexture);
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "           BaseColorTex: " << (tex ? "OK" : "FAILED") << "\n";
#endif
            }
            if (mat->normalTexture.isValid())
            {
                Engine::TextureAsset *tex = assets.getTexture(mat->normalTexture);
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "           NormalTex: " << (tex ? "OK" : "FAILED") << "\n";
#endif
            }
        }

        // Validate node graph if present
        if (!model->nodes.empty())
        {
            const auto &nodes = model->nodes;
            const auto &np = model->nodePrimitiveIndices;
            const auto &nc = model->nodeChildIndices;
            const uint32_t nodeCount = static_cast<uint32_t>(nodes.size());
            const uint32_t primCount = static_cast<uint32_t>(model->primitives.size());

            auto nearlyEqual = [](float a, float b)
            {
                return std::abs(a - b) < 1e-4f;
            };

            bool nodeOk = true;
            std::cout << "[SMODEL] Nodes=" << nodeCount << " NodePrimIx=" << np.size() << "\n";

            for (uint32_t i = 0; i < nodeCount; ++i)
            {
                const auto &n = nodes[i];

                // Bounds checks on hierarchy and primitive ranges
                if (n.parentIndex != ~0u && n.parentIndex >= nodeCount)
                {
                    std::cerr << "  Node[" << i << "] invalid parentIndex=" << n.parentIndex << "\n";
                    nodeOk = false;
                }
                if (n.childCount > 0)
                {
                    if (n.firstChildIndex == ~0u || (size_t(n.firstChildIndex) + size_t(n.childCount) > nc.size()))
                    {
                        std::cerr << "  Node[" << i << "] invalid child range firstChildIndex=" << n.firstChildIndex << " count=" << n.childCount << "\n";
                        nodeOk = false;
                    }
                    else
                    {
                        for (uint32_t k = 0; k < n.childCount; ++k)
                        {
                            const uint32_t c = nc[n.firstChildIndex + k];
                            if (c >= nodeCount)
                            {
                                std::cerr << "  Node[" << i << "] child index out of bounds: " << c << "\n";
                                nodeOk = false;
                                break;
                            }
                            if (nodes[c].parentIndex != i)
                            {
                                std::cerr << "  Node[" << i << "] child parent mismatch child=" << c << " parentIndex=" << nodes[c].parentIndex << "\n";
                                nodeOk = false;
                                break;
                            }
                        }
                    }
                }
                if (n.primitiveCount > 0)
                {
                    if (n.firstPrimitiveIndex + n.primitiveCount > np.size())
                    {
                        std::cerr << "  Node[" << i << "] invalid prim range first=" << n.firstPrimitiveIndex << " count=" << n.primitiveCount << "\n";
                        nodeOk = false;
                    }
                    for (uint32_t k = 0; k < n.primitiveCount && (n.firstPrimitiveIndex + k) < np.size(); ++k)
                    {
                        const uint32_t pidx = np[n.firstPrimitiveIndex + k];
                        if (pidx >= primCount)
                        {
                            std::cerr << "  Node[" << i << "] prim ref out of bounds: " << pidx << "\n";
                            nodeOk = false;
                        }
                    }
                }

                // Global validation done after we compute expected globals via traversal.
            }

            // Validate globals via DFS (ordering-independent).
            {
                std::vector<glm::mat4> expectedGlobals(nodeCount, glm::mat4(1.0f));
                std::vector<uint8_t> visited(nodeCount, 0);

                std::function<void(uint32_t, const glm::mat4 &)> dfs = [&](uint32_t idx, const glm::mat4 &parent)
                {
                    if (idx >= nodeCount)
                        return;
                    if (visited[idx])
                        return;
                    visited[idx] = 1;

                    const auto &n = nodes[idx];
                    expectedGlobals[idx] = parent * n.localMatrix;

                    if (n.childCount == 0 || n.firstChildIndex == ~0u)
                        return;
                    if (size_t(n.firstChildIndex) + size_t(n.childCount) > nc.size())
                        return;

                    for (uint32_t k = 0; k < n.childCount; ++k)
                    {
                        const uint32_t c = nc[n.firstChildIndex + k];
                        dfs(c, expectedGlobals[idx]);
                    }
                };

                for (uint32_t i = 0; i < nodeCount; ++i)
                {
                    if (nodes[i].parentIndex == ~0u)
                        dfs(i, glm::mat4(1.0f));
                }

                for (uint32_t i = 0; i < nodeCount; ++i)
                {
                    if (!visited[i])
                        continue;

                    const float *a = glm::value_ptr(expectedGlobals[i]);
                    const float *b = glm::value_ptr(nodes[i].globalMatrix);
                    for (int m = 0; m < 16; ++m)
                    {
                        if (!nearlyEqual(a[m], b[m]))
                        {
                            std::cerr << "  Node[" << i << "] global mismatch at element " << m << " expected=" << a[m] << " got=" << b[m] << "\n";
                            nodeOk = false;
                            break;
                        }
                    }
                }
            }

            if (nodeOk)
                std::cout << "[SMODEL] Node graph validation OK\n";
        }
        else
        {
            std::cout << "[SMODEL] No nodes present (fallback primitive-only)\n";
        }

        std::cout << "[SMODEL] Verification complete \n\n";
        return modelHandle;
    }
}
