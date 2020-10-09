#include "chrono_vsg/shapes/VSGCylinder.h"
#include "chrono_thirdparty/stb/stb_image.h"
#include "chrono_thirdparty/filesystem/path.h"

using namespace chrono::vsg3d;

VSGCylinder::VSGCylinder(std::shared_ptr<ChBody> body,
                         std::shared_ptr<ChAsset> asset,
                         vsg::ref_ptr<vsg::MatrixTransform> transform)
    : ChVSGIdxMesh(body, asset, transform) {}

void VSGCylinder::Initialize(vsg::vec3& lightPosition, ChVSGPhongMaterial& mat, std::string& texFilePath) {
    m_lightPosition = lightPosition;
    m_textureFilePath = texFilePath;

    // set up vertices, normals, texcoords, indices
    m_vertices = vsg::vec3Array::create({
        {1, 0, 0.5},
        {0.951057, 0.309017, 0.5},
        {0.809017, 0.587785, 0.5},
        {0.587785, 0.809017, 0.5},
        {0.309017, 0.951057, 0.5},
        {6.12323e-17, 1, 0.5},
        {-0.309017, 0.951057, 0.5},
        {-0.587785, 0.809017, 0.5},
        {-0.809017, 0.587785, 0.5},
        {-0.951057, 0.309017, 0.5},
        {-1, 1.22465e-16, 0.5},
        {-0.951057, -0.309017, 0.5},
        {-0.809017, -0.587785, 0.5},
        {-0.587785, -0.809017, 0.5},
        {-0.309017, -0.951057, 0.5},
        {-1.83697e-16, -1, 0.5},
        {0.309017, -0.951057, 0.5},
        {0.587785, -0.809017, 0.5},
        {0.809017, -0.587785, 0.5},
        {0.951057, -0.309017, 0.5},
        {1, -2.44929e-16, 0.5},
        {1, 0, -0.5},
        {0.951057, 0.309017, -0.5},
        {0.809017, 0.587785, -0.5},
        {0.587785, 0.809017, -0.5},
        {0.309017, 0.951057, -0.5},
        {6.12323e-17, 1, -0.5},
        {-0.309017, 0.951057, -0.5},
        {-0.587785, 0.809017, -0.5},
        {-0.809017, 0.587785, -0.5},
        {-0.951057, 0.309017, -0.5},
        {-1, 1.22465e-16, -0.5},
        {-0.951057, -0.309017, -0.5},
        {-0.809017, -0.587785, -0.5},
        {-0.587785, -0.809017, -0.5},
        {-0.309017, -0.951057, -0.5},
        {-1.83697e-16, -1, -0.5},
        {0.309017, -0.951057, -0.5},
        {0.587785, -0.809017, -0.5},
        {0.809017, -0.587785, -0.5},
        {0.951057, -0.309017, -0.5},
        {1, -2.44929e-16, -0.5},
        {1, 0, 0.5},
        {0.951057, 0.309017, 0.5},
        {0.809017, 0.587785, 0.5},
        {0.587785, 0.809017, 0.5},
        {0.309017, 0.951057, 0.5},
        {6.12323e-17, 1, 0.5},
        {-0.309017, 0.951057, 0.5},
        {-0.587785, 0.809017, 0.5},
        {-0.809017, 0.587785, 0.5},
        {-0.951057, 0.309017, 0.5},
        {-1, 1.22465e-16, 0.5},
        {-0.951057, -0.309017, 0.5},
        {-0.809017, -0.587785, 0.5},
        {-0.587785, -0.809017, 0.5},
        {-0.309017, -0.951057, 0.5},
        {-1.83697e-16, -1, 0.5},
        {0.309017, -0.951057, 0.5},
        {0.587785, -0.809017, 0.5},
        {0.809017, -0.587785, 0.5},
        {0.951057, -0.309017, 0.5},
        {1, -2.44929e-16, 0.5},
        {1, 0, -0.5},
        {0.951057, 0.309017, -0.5},
        {0.809017, 0.587785, -0.5},
        {0.587785, 0.809017, -0.5},
        {0.309017, 0.951057, -0.5},
        {6.12323e-17, 1, -0.5},
        {-0.309017, 0.951057, -0.5},
        {-0.587785, 0.809017, -0.5},
        {-0.809017, 0.587785, -0.5},
        {-0.951057, 0.309017, -0.5},
        {-1, 1.22465e-16, -0.5},
        {-0.951057, -0.309017, -0.5},
        {-0.809017, -0.587785, -0.5},
        {-0.587785, -0.809017, -0.5},
        {-0.309017, -0.951057, -0.5},
        {-1.83697e-16, -1, -0.5},
        {0.309017, -0.951057, -0.5},
        {0.587785, -0.809017, -0.5},
        {0.809017, -0.587785, -0.5},
        {0.951057, -0.309017, -0.5},
        {1, -2.44929e-16, -0.5},
        {0, 0, 0.5},
        {0, 0, -0.5},
    });

    m_normals = vsg::vec3Array::create({
        {1, 0, 0},
        {0.951057, 0.309017, 0},
        {0.809017, 0.587785, 0},
        {0.587785, 0.809017, 0},
        {0.309017, 0.951057, 0},
        {6.12323e-17, 1, 0},
        {-0.309017, 0.951057, 0},
        {-0.587785, 0.809017, 0},
        {-0.809017, 0.587785, 0},
        {-0.951057, 0.309017, 0},
        {-1, 1.22465e-16, 0},
        {-0.951057, -0.309017, 0},
        {-0.809017, -0.587785, 0},
        {-0.587785, -0.809017, 0},
        {-0.309017, -0.951057, 0},
        {-1.83697e-16, -1, 0},
        {0.309017, -0.951057, 0},
        {0.587785, -0.809017, 0},
        {0.809017, -0.587785, 0},
        {0.951057, -0.309017, 0},
        {1, -2.44929e-16, 0},
        {1, 0, 0},
        {0.951057, 0.309017, 0},
        {0.809017, 0.587785, 0},
        {0.587785, 0.809017, 0},
        {0.309017, 0.951057, 0},
        {6.12323e-17, 1, 0},
        {-0.309017, 0.951057, 0},
        {-0.587785, 0.809017, 0},
        {-0.809017, 0.587785, 0},
        {-0.951057, 0.309017, 0},
        {-1, 1.22465e-16, 0},
        {-0.951057, -0.309017, 0},
        {-0.809017, -0.587785, 0},
        {-0.587785, -0.809017, 0},
        {-0.309017, -0.951057, 0},
        {-1.83697e-16, -1, 0},
        {0.309017, -0.951057, 0},
        {0.587785, -0.809017, 0},
        {0.809017, -0.587785, 0},
        {0.951057, -0.309017, 0},
        {1, -2.44929e-16, 0},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, -1},
        {0, 0, 1},
        {0, 0, -1},
    });

    m_texcoords = vsg::vec2Array::create({
        {0, 0.666667},    {0.05, 0.666667}, {0.1, 0.666667},  {0.15, 0.666667}, {0.2, 0.666667},  {0.25, 0.666667},
        {0.3, 0.666667},  {0.35, 0.666667}, {0.4, 0.666667},  {0.45, 0.666667}, {0.5, 0.666667},  {0.55, 0.666667},
        {0.6, 0.666667},  {0.65, 0.666667}, {0.7, 0.666667},  {0.75, 0.666667}, {0.8, 0.666667},  {0.85, 0.666667},
        {0.9, 0.666667},  {0.95, 0.666667}, {1, 0.666667},    {0, 0.333333},    {0.05, 0.333333}, {0.1, 0.333333},
        {0.15, 0.333333}, {0.2, 0.333333},  {0.25, 0.333333}, {0.3, 0.333333},  {0.35, 0.333333}, {0.4, 0.333333},
        {0.45, 0.333333}, {0.5, 0.333333},  {0.55, 0.333333}, {0.6, 0.333333},  {0.65, 0.333333}, {0.7, 0.333333},
        {0.75, 0.333333}, {0.8, 0.333333},  {0.85, 0.333333}, {0.9, 0.333333},  {0.95, 0.333333}, {1, 0.333333},
        {0, 0.666667},    {0.05, 0.666667}, {0.1, 0.666667},  {0.15, 0.666667}, {0.2, 0.666667},  {0.25, 0.666667},
        {0.3, 0.666667},  {0.35, 0.666667}, {0.4, 0.666667},  {0.45, 0.666667}, {0.5, 0.666667},  {0.55, 0.666667},
        {0.6, 0.666667},  {0.65, 0.666667}, {0.7, 0.666667},  {0.75, 0.666667}, {0.8, 0.666667},  {0.85, 0.666667},
        {0.9, 0.666667},  {0.95, 0.666667}, {1, 0.666667},    {0, 0.666667},    {0.05, 0.666667}, {0.1, 0.666667},
        {0.15, 0.666667}, {0.2, 0.666667},  {0.25, 0.666667}, {0.3, 0.666667},  {0.35, 0.666667}, {0.4, 0.666667},
        {0.45, 0.666667}, {0.5, 0.666667},  {0.55, 0.666667}, {0.6, 0.666667},  {0.65, 0.666667}, {0.7, 0.666667},
        {0.75, 0.666667}, {0.8, 0.666667},  {0.85, 0.666667}, {0.9, 0.666667},  {0.95, 0.666667}, {1, 0.666667},
        {0.5, 1},         {0.5, 0},
    });

    m_indices = vsg::ushortArray::create({
        0,  21, 22, 0,  22, 1,  1,  22, 23, 1,  23, 2,  2,  23, 24, 2,  24, 3,  3,  24, 25, 3,  25, 4,  4,  25, 26,
        4,  26, 5,  5,  26, 27, 5,  27, 6,  6,  27, 28, 6,  28, 7,  7,  28, 29, 7,  29, 8,  8,  29, 30, 8,  30, 9,
        9,  30, 31, 9,  31, 10, 10, 31, 32, 10, 32, 11, 11, 32, 33, 11, 33, 12, 12, 33, 34, 12, 34, 13, 13, 34, 35,
        13, 35, 14, 14, 35, 36, 14, 36, 15, 15, 36, 37, 15, 37, 16, 16, 37, 38, 16, 38, 17, 17, 38, 39, 17, 39, 18,
        18, 39, 40, 18, 40, 19, 19, 40, 41, 19, 41, 20, 84, 42, 43, 84, 43, 44, 84, 44, 45, 84, 45, 46, 84, 46, 47,
        84, 47, 48, 84, 48, 49, 84, 49, 50, 84, 50, 51, 84, 51, 52, 84, 52, 53, 84, 53, 54, 84, 54, 55, 84, 55, 56,
        84, 56, 57, 84, 57, 58, 84, 58, 59, 84, 59, 60, 84, 60, 61, 84, 61, 62, 85, 64, 63, 85, 65, 64, 85, 66, 65,
        85, 67, 66, 85, 68, 67, 85, 69, 68, 85, 70, 69, 85, 71, 70, 85, 72, 71, 85, 73, 72, 85, 74, 73, 85, 75, 74,
        85, 76, 75, 85, 77, 76, 85, 78, 77, 85, 79, 78, 85, 80, 79, 85, 81, 80, 85, 82, 81, 85, 83, 82,
    });

    m_ambientColor = vsg::vec3Array::create(m_vertices->size(), mat.ambientColor);
    m_diffuseColor = vsg::vec3Array::create(m_vertices->size(), mat.diffuseColor);
    m_specularColor = vsg::vec3Array::create(m_vertices->size(), mat.specularColor);
    m_shininess = vsg::floatArray::create(m_vertices->size(), mat.shininess);
    m_opacity = vsg::floatArray::create(m_vertices->size(), mat.opacity);
}