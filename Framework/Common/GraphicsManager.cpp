#include <iostream>
#include "GraphicsManager.hpp"
#include "SceneManager.hpp"
#include "IApplication.hpp"
#include "IPhysicsManager.hpp"
#include "ForwardRenderPass.hpp"
#include "ShadowMapPass.hpp"
#include "HUDPass.hpp"
#include "SkyBoxPass.hpp"
#include "BRDFIntegrator.hpp"

using namespace My;
using namespace std;

int GraphicsManager::Initialize()
{
    int result = 0;
    m_Frames.resize(kFrameCount);
    m_InitPasses.push_back(make_shared<BRDFIntegrator>());

	InitConstants();
    m_DrawPasses.push_back(make_shared<ShadowMapPass>());
    m_DrawPasses.push_back(make_shared<ForwardRenderPass>());
    m_DrawPasses.push_back(make_shared<SkyBoxPass>());
    m_DrawPasses.push_back(make_shared<HUDPass>());
    return result;
}

void GraphicsManager::Finalize()
{
#ifdef DEBUG
    ClearDebugBuffers();
#endif
    ClearBuffers();
}

void GraphicsManager::Tick()
{
    if (g_pSceneManager->IsSceneChanged())
    {
        cerr << "[GraphicsManager] Detected Scene Change, reinitialize buffers ..." << endl;
        ClearBuffers();
        const Scene& scene = g_pSceneManager->GetSceneForRendering();
        InitializeBuffers(scene);
        g_pSceneManager->NotifySceneIsRenderingQueued();
    }

    UpdateConstants();

    Clear();
    Draw();
}

void GraphicsManager::UpdateConstants()
{
    // update scene object position
    auto& frame = m_Frames[m_nFrameIndex];

    for (auto dbc : frame.batchContexts)
    {
        if (void* rigidBody = dbc->node->RigidBody()) {
            Matrix4X4f trans;

            // the geometry has rigid body bounded, we blend the simlation result here.
            Matrix4X4f simulated_result = g_pPhysicsManager->GetRigidBodyTransform(rigidBody);

            BuildIdentityMatrix(trans);

            // apply the rotation part of the simlation result
            memcpy(trans[0], simulated_result[0], sizeof(float) * 3);
            memcpy(trans[1], simulated_result[1], sizeof(float) * 3);
            memcpy(trans[2], simulated_result[2], sizeof(float) * 3);

            // replace the translation part of the matrix with simlation result directly
            memcpy(trans[3], simulated_result[3], sizeof(float) * 3);

            dbc->trans = trans;
        } else {
            dbc->trans = *dbc->node->GetCalculatedTransform();
        }
    }

    // Generate the view matrix based on the camera's position.
    CalculateCameraMatrix();
    CalculateLights();
}

void GraphicsManager::Clear()
{

}

void GraphicsManager::Draw()
{
    auto& frame = m_Frames[m_nFrameIndex];

    for (auto pDrawPass : m_DrawPasses)
    {
        pDrawPass->Draw(frame);
    }

#ifdef DEBUG
    RenderDebugBuffers();
#endif
}

void GraphicsManager::InitConstants()
{
}

void GraphicsManager::CalculateCameraMatrix()
{
    auto& scene = g_pSceneManager->GetSceneForRendering();
    auto pCameraNode = scene.GetFirstCameraNode();
    DrawFrameContext& frameContext = m_Frames[m_nFrameIndex].frameContext;
    if (pCameraNode) {
        auto transform = *pCameraNode->GetCalculatedTransform();
        frameContext.m_camPos = Vector3f({transform[3][0], transform[3][1], transform[3][2]});
        InverseMatrix4X4f(transform);
        frameContext.m_viewMatrix = transform;
    }
    else {
        // use default build-in camera
        Vector3f position = { 0.0f, -5.0f, 0.0f }, lookAt = { 0.0f, 0.0f, 0.0f }, up = { 0.0f, 0.0f, 1.0f };
        BuildViewRHMatrix(frameContext.m_viewMatrix, position, lookAt, up);
    }

    float fieldOfView = PI / 3.0f;
    float nearClipDistance = 1.0f;
    float farClipDistance = 100.0f;

    if (pCameraNode) {
        auto pCamera = scene.GetCamera(pCameraNode->GetSceneObjectRef());
        // Set the field of view and screen aspect ratio.
        fieldOfView = dynamic_pointer_cast<SceneObjectPerspectiveCamera>(pCamera)->GetFov();
        nearClipDistance = pCamera->GetNearClipDistance();
        farClipDistance = pCamera->GetFarClipDistance();
    }

    const GfxConfiguration& conf = g_pApp->GetConfiguration();

    float screenAspect = (float)conf.screenWidth / (float)conf.screenHeight;

    // Build the perspective projection matrix.
    BuildPerspectiveFovRHMatrix(frameContext.m_projectionMatrix, fieldOfView, screenAspect, nearClipDistance, farClipDistance);
}

void GraphicsManager::CalculateLights()
{
    DrawFrameContext& frameContext = m_Frames[m_nFrameIndex].frameContext;
    frameContext.m_ambientColor = { 0.01f, 0.01f, 0.01f };
    frameContext.m_lights.clear();

    auto& scene = g_pSceneManager->GetSceneForRendering();
    for (auto LightNode : scene.LightNodes) {
        Light& light = *(new Light());
        auto pLightNode = LightNode.second.lock();
        if (!pLightNode) continue;
        auto trans_ptr = pLightNode->GetCalculatedTransform();
        light.lightPosition = { 0.0f, 0.0f, 0.0f, 1.0f };
        light.lightDirection = { 0.0f, 0.0f, -1.0f, 0.0f };
        Transform(light.lightPosition, *trans_ptr);
        Transform(light.lightDirection, *trans_ptr);

        auto pLight = scene.GetLight(pLightNode->GetSceneObjectRef());
        if (pLight) {
            light.lightGuid = pLight->GetGuid();
            light.lightColor = pLight->GetColor().Value;
            light.lightIntensity = pLight->GetIntensity();
            light.lightCastShadow = pLight->GetIfCastShadow();
            const AttenCurve& atten_curve = pLight->GetDistanceAttenuation();
            light.lightDistAttenCurveType = atten_curve.type; 
            memcpy(light.lightDistAttenCurveParams, &atten_curve.u, sizeof(atten_curve.u));
            light.lightAngleAttenCurveType = AttenCurveType::kNone;

            Matrix4X4f view;
            Matrix4X4f projection;
            BuildIdentityMatrix(projection);

            float nearClipDistance = 1.0f;
            float farClipDistance = 100.0f;

            if (pLight->GetType() == SceneObjectType::kSceneObjectTypeLightInfi)
            {
                light.lightType = LightType::Infinity;

                Vector4f target = { 0.0f, 0.0f, 0.0f, 1.0f };

                auto pCameraNode = scene.GetFirstCameraNode();
                if (pCameraNode) {
                    auto pCamera = scene.GetCamera(pCameraNode->GetSceneObjectRef());
                    nearClipDistance = pCamera->GetNearClipDistance();
                    farClipDistance = pCamera->GetFarClipDistance();

                    target[2] = - (0.75f * nearClipDistance + 0.25f * farClipDistance);

                    // calculate the camera target position
                    auto trans_ptr = pCameraNode->GetCalculatedTransform();
                    Transform(target, *trans_ptr);
                }

                light.lightPosition = target - light.lightDirection * farClipDistance;
                Vector3f position;
                memcpy(&position, &light.lightPosition, sizeof position); 
                Vector3f lookAt; 
                memcpy(&lookAt, &target, sizeof lookAt);
                Vector3f up = { 0.0f, 0.0f, 1.0f };
                if (abs(light.lightDirection[0]) <= 0.2f
                    && abs(light.lightDirection[1]) <= 0.2f)
                {
                    up = { 0.1f, 0.1f, 1.0f};
                }
                BuildViewRHMatrix(view, position, lookAt, up);

                float sm_half_dist = min(farClipDistance * 0.25f, 800.0f);

                BuildOrthographicMatrix(projection, 
                    - sm_half_dist, sm_half_dist, 
                    sm_half_dist, - sm_half_dist, 
                    nearClipDistance, farClipDistance + sm_half_dist);

                // notify shader about the infinity light by setting 4th field to 0
                light.lightPosition[3] = 0.0f;
            }
            else 
            {
                Vector3f position;
                memcpy(&position, &light.lightPosition, sizeof position); 
                Vector4f tmp = light.lightPosition + light.lightDirection;
                Vector3f lookAt; 
                memcpy(&lookAt, &tmp, sizeof lookAt);
                Vector3f up = { 0.0f, 0.0f, 1.0f };
                if (abs(light.lightDirection[0]) <= 0.1f
                    && abs(light.lightDirection[1]) <= 0.1f)
                {
                    up = { 0.0f, 0.707f, 0.707f};
                }
                BuildViewRHMatrix(view, position, lookAt, up);

                if (pLight->GetType() == SceneObjectType::kSceneObjectTypeLightSpot)
                {
                    light.lightType = LightType::Spot;

                    auto plight = dynamic_pointer_cast<SceneObjectSpotLight>(pLight);
                    const AttenCurve& angle_atten_curve = plight->GetAngleAttenuation();
                    light.lightAngleAttenCurveType = angle_atten_curve.type;
                    memcpy(light.lightAngleAttenCurveParams, &angle_atten_curve.u, sizeof(angle_atten_curve.u));

                    float fieldOfView = light.lightAngleAttenCurveParams[0][1] * 2.0f;
                    float screenAspect = 1.0f;

                    // Build the perspective projection matrix.
                    BuildPerspectiveFovRHMatrix(projection, fieldOfView, screenAspect, nearClipDistance, farClipDistance);
                }
                else if (pLight->GetType() == SceneObjectType::kSceneObjectTypeLightArea)
                {
                    light.lightType = LightType::Area;

                    auto plight = dynamic_pointer_cast<SceneObjectAreaLight>(pLight);
                    light.lightSize = plight->GetDimension();
                }
                else // omni light
                {
                    light.lightType = LightType::Omni;

                    //auto plight = dynamic_pointer_cast<SceneObjectOmniLight>(pLight);

                    float fieldOfView = PI / 2.0f; // 90 degree for each cube map face
                    float screenAspect = 1.0f;

                    // Build the perspective projection matrix.
                    BuildPerspectiveFovRHMatrix(projection, fieldOfView, screenAspect, nearClipDistance, farClipDistance);
                }
            } 

            light.lightVP = view * projection;
        }
        else
        {
            assert(0);
        }

        frameContext.m_lights.push_back(std::move(light));
    }
}

void GraphicsManager::InitializeBuffers(const Scene& scene)
{
    for (auto pPass : m_InitPasses)
    {
        pPass->Dispatch();
    }
}

void GraphicsManager::ClearBuffers()
{
    cerr << "[GraphicsManager] ClearBuffers()" << endl;
}

void GraphicsManager::SetSkyBox(const DrawFrameContext& context)
{
    cerr << "[GraphicsManager] SetSkyBox(" << &context << ")" << endl;
}

void GraphicsManager::DrawSkyBox()
{
    cerr << "[GraphicsManager] DrawSkyBox()" << endl;
}

#ifdef DEBUG
void GraphicsManager::RenderDebugBuffers()
{
    cerr << "[GraphicsManager] RenderDebugBuffers()" << endl;
}

void GraphicsManager::DrawPoint(const Point& point, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawPoint(" << point << ", "
        << color << ")" << endl;
}

void GraphicsManager::DrawPointSet(const PointSet& point_set, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawPointSet(" << point_set.size() << ", "
        << color << ")" << endl;
}

void GraphicsManager::DrawPointSet(const PointSet& point_set, const Matrix4X4f& trans, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawPointSet(" << point_set.size() << ", "
        << trans << "," 
        << color << ")" << endl;
}

void GraphicsManager::DrawLine(const Point& from, const Point& to, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawLine(" << from << ", "
        << to << ", " 
        << color << ")" << endl;
}

void GraphicsManager::DrawLine(const PointList& vertices, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawLine(" << vertices.size() << ", "
        << color << ")" << endl;
}

void GraphicsManager::DrawLine(const PointList& vertices, const Matrix4X4f& trans, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawLine(" << vertices.size() << ", "
        << trans << ", " 
        << color << ")" << endl;
}

void GraphicsManager::DrawEdgeList(const EdgeList& edges, const Vector3f& color)
{
    PointList point_list;

    for (auto edge : edges)
    {
        point_list.push_back(edge->first);
        point_list.push_back(edge->second);
    }

    DrawLine(point_list, color);
}

void GraphicsManager::DrawTriangle(const PointList& vertices, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawTriangle(" << vertices.size() << ", "
        << color << ")" << endl;
}

void GraphicsManager::DrawTriangle(const PointList& vertices, const Matrix4X4f& trans, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawTriangle(" << vertices.size() << ", "
        << color << ")" << endl;
}

void GraphicsManager::DrawTriangleStrip(const PointList& vertices, const Vector3f& color)
{
    cerr << "[GraphicsManager] DrawTriangleStrip(" << vertices.size() << ", "
        << color << ")" << endl;
}

void GraphicsManager::DrawPolygon(const Face& polygon, const Vector3f& color)
{
    PointSet vertices;
    PointList edges;
    for (auto pEdge : polygon.Edges)
    {
        vertices.insert({pEdge->first, pEdge->second});
        edges.push_back(pEdge->first);
        edges.push_back(pEdge->second);
    }
    DrawLine(edges, color);

    DrawPointSet(vertices, color);

    DrawTriangle(polygon.GetVertices(), color * 0.5f);
}

void GraphicsManager::DrawPolygon(const Face& polygon, const Matrix4X4f& trans, const Vector3f& color)
{
    PointSet vertices;
    PointList edges;
    for (auto pEdge : polygon.Edges)
    {
        vertices.insert({pEdge->first, pEdge->second});
        edges.push_back(pEdge->first);
        edges.push_back(pEdge->second);
    }
    DrawLine(edges, trans, color);

    DrawPointSet(vertices, trans, color);

    DrawTriangle(polygon.GetVertices(), trans, color * 0.5f);
}

void GraphicsManager::DrawPolyhydron(const Polyhedron& polyhedron, const Vector3f& color)
{
    for (auto pFace : polyhedron.Faces)
    {
        DrawPolygon(*pFace, color);
    }
}

void GraphicsManager::DrawPolyhydron(const Polyhedron& polyhedron, const Matrix4X4f& trans, const Vector3f& color)
{
    for (auto pFace : polyhedron.Faces)
    {
        DrawPolygon(*pFace, trans, color);
    }
}

void GraphicsManager::DrawBox(const Vector3f& bbMin, const Vector3f& bbMax, const Vector3f& color)
{
    //  ******0--------3********
    //  *****/:       /|********
    //  ****1--------2 |********
    //  ****| :      | |********
    //  ****| 4- - - | 7********
    //  ****|/       |/*********
    //  ****5--------6**********

    // vertices
    PointPtr points[8];
    for (int i = 0; i < 8; i++)
        points[i] = make_shared<Point>(bbMin);
    *points[0] = *points[2] = *points[3] = *points[7] = bbMax;
    points[0]->data[0] = bbMin[0];
    points[2]->data[1] = bbMin[1];
    points[7]->data[2] = bbMin[2];
    points[1]->data[2] = bbMax[2];
    points[4]->data[1] = bbMax[1];
    points[6]->data[0] = bbMax[0];

    // edges
    EdgeList edges;
    
    // top
    edges.push_back(make_shared<Edge>(make_pair(points[0], points[3])));
    edges.push_back(make_shared<Edge>(make_pair(points[3], points[2])));
    edges.push_back(make_shared<Edge>(make_pair(points[2], points[1])));
    edges.push_back(make_shared<Edge>(make_pair(points[1], points[0])));

    // bottom
    edges.push_back(make_shared<Edge>(make_pair(points[4], points[7])));
    edges.push_back(make_shared<Edge>(make_pair(points[7], points[6])));
    edges.push_back(make_shared<Edge>(make_pair(points[6], points[5])));
    edges.push_back(make_shared<Edge>(make_pair(points[5], points[4])));

    // side
    edges.push_back(make_shared<Edge>(make_pair(points[0], points[4])));
    edges.push_back(make_shared<Edge>(make_pair(points[1], points[5])));
    edges.push_back(make_shared<Edge>(make_pair(points[2], points[6])));
    edges.push_back(make_shared<Edge>(make_pair(points[3], points[7])));

    DrawEdgeList(edges, color);
}

void GraphicsManager::ClearDebugBuffers()
{
    cerr << "[GraphicsManager] ClearDebugBuffers(void)" << endl;
}

void GraphicsManager::DrawTextureOverlay(const intptr_t texture, 
    float vp_left, float vp_top, float vp_width, float vp_height)
{
    cerr << "[GraphicsManager] DrayOverlay(" << texture << ", "
        << vp_left << ", "
        << vp_top << ", "
        << vp_width << ", "
        << vp_height << ", "
        << ")" << endl;
}

void GraphicsManager::DrawTextureArrayOverlay(const intptr_t texture, uint32_t layer_index, 
    float vp_left, float vp_top, float vp_width, float vp_height)
{
    cerr << "[GraphicsManager] DrayOverlay(" << texture << ", "
        << layer_index << ", "
        << vp_left << ", "
        << vp_top << ", "
        << vp_width << ", "
        << vp_height << ", "
        << ")" << endl;
}

void GraphicsManager::DrawCubeMapOverlay(const intptr_t cubemap, 
    float vp_left, float vp_top, float vp_width, float vp_height, float level)
{
    cerr << "[GraphicsManager] DrayCubeMapOverlay(" << cubemap << ", "
        << vp_left << ", "
        << vp_top << ", "
        << vp_width << ", "
        << vp_height << ", "
        << level << ", "
        << ")" << endl;
}

void GraphicsManager::DrawCubeMapArrayOverlay(const intptr_t cubemap, uint32_t layer_index, 
    float vp_left, float vp_top, float vp_width, float vp_height, float level)
{
    cerr << "[GraphicsManager] DrayCubeMapOverlay(" << cubemap << ", "
        << layer_index << ", "
        << vp_left << ", "
        << vp_top << ", "
        << vp_width << ", "
        << vp_height << ", "
        << level << ", "
        << ")" << endl;
}

#endif

void GraphicsManager::UseShaderProgram(const intptr_t shaderProgram)
{
    cerr << "[GraphicsManager] UseShaderProgram(" << shaderProgram << ")" << endl;
}

void GraphicsManager::SetPerFrameConstants(const DrawFrameContext& context)
{
    cerr << "[GraphicsManager] SetPerFrameConstants(" << &context << ")" << endl;
}

void GraphicsManager::SetPerBatchConstants(const DrawBatchContext& context)
{
    cout << "[GraphicsManager] SetPerBatchConstants(" << &context << ")" << endl;
}

void GraphicsManager::DrawBatch(const DrawBatchContext& context)
{
    cerr << "[GraphicsManager] DrawBatch(" << &context << ")" << endl;
}

void GraphicsManager::DrawBatchDepthOnly(const DrawBatchContext& context)
{
    cerr << "[GraphicsManager] DrawBatchDepthOnly(" << &context << ")" << endl;
}

intptr_t GraphicsManager::GenerateCubeShadowMapArray(const uint32_t width, const uint32_t height, const uint32_t count)
{
    cerr << "[GraphicsManager] GenerateCubeShadowMapArray(" << width << ", " << height << ","
        << count << ")" << endl;
    return 0;
}

intptr_t GraphicsManager::GenerateShadowMapArray(const uint32_t width, const uint32_t height, const uint32_t count)
{
    cerr << "[GraphicsManager] GenerateShadowMapArray(" << width << " ," << height << ", " 
        << count << ")" << endl;
    return 0;
}

void GraphicsManager::BeginShadowMap(const Light& light, const intptr_t shadowmap, const uint32_t width, const uint32_t height, const uint32_t layer_index)
{
    cout << "[GraphicsManager] BeginShadowMap(" << light.lightGuid << ", " << shadowmap << ", " 
        << width << ", " << height << ", " << layer_index << ")" << endl;
}

void GraphicsManager::EndShadowMap(const intptr_t shadowmap, const uint32_t layer_index)
{
    cerr << "[GraphicsManager] EndShadowMap(" << shadowmap << ", " << layer_index << ")" << endl;
}

void GraphicsManager::SetShadowMaps(const Frame& frame)
{
    cerr << "[GraphicsManager] SetShadowMap(" << &frame << ")" << endl;
}

void GraphicsManager::DestroyShadowMap(intptr_t& shadowmap)
{
    cerr << "[GraphicsManager] DestroyShadowMap(" << shadowmap << ")" << endl;
    shadowmap = -1;
}

intptr_t GraphicsManager::GenerateAndBindTextureForWrite(const char* id, const uint32_t width, const uint32_t height)
{
    cerr << "[GraphicsManager] GenerateAndBindTextureForWrite(" << id << ", " << width << ", " << height << ")" << endl;
    return static_cast<intptr_t>(0);
}

void GraphicsManager::Dispatch(const uint32_t width, const uint32_t height, const uint32_t depth)
{
    cerr << "[GraphicsManager] Dispatch(" << width << ", " << height << ", " << depth << ")" << endl;
}

intptr_t GraphicsManager::GetTexture(const char* id)
{
    cerr << "[GraphicsManager] GetTexture(" << id << ")" << endl;
    return static_cast<intptr_t>(0);
}

intptr_t GraphicsManager::GenerateTexture(const char* id, const uint32_t width, const uint32_t height)
{
    cerr << "[GraphicsManager] GenerateTexture(" << id << ", " << width << ", " << height << ")" << endl;
    return static_cast<intptr_t>(0);
}

void GraphicsManager::BeginRenderToTexture(intptr_t& context, const intptr_t texture, const uint32_t width, const uint32_t height)
{
    cerr << "[GraphicsManager] BeginRenderToTexture(" << context << ", " << texture << ", " << width << ", " << height << ")" << endl;
    context = 0;
}

void GraphicsManager::EndRenderToTexture(intptr_t& context)
{
    cerr << "[GraphicsManager] EndRenderToTexture(" << context << ")" << endl;
}

void GraphicsManager::DrawFullScreenQuad()
{
    cerr << "[GraphicsManager] DrawFullScreenQuad()" << endl;
}

bool GraphicsManager::CheckCapability(RHICapability cap)
{
    return false;
}
