#include "chrono_vsg/shapes/ChVSGSimplePhongIdxMesh.h"

#include "chrono_thirdparty/stb/stb_image.h"
#include "chrono_thirdparty/filesystem/path.h"

using namespace chrono::vsg3d;

ChVSGSimplePhongIdxMesh::ChVSGSimplePhongIdxMesh(std::shared_ptr<ChBody> body,
                                                 std::shared_ptr<ChAsset> asset,
                                                 vsg::ref_ptr<vsg::MatrixTransform> transform)
    : m_bodyPtr(body), m_assetPtr(asset), m_transform(transform) {}

vsg::ref_ptr<vsg::ShaderStage> ChVSGSimplePhongIdxMesh::readVertexShader(std::string filePath) {
    return vsg::ShaderStage::read(VK_SHADER_STAGE_VERTEX_BIT, "main", filePath);
}

vsg::ref_ptr<vsg::ShaderStage> ChVSGSimplePhongIdxMesh::readFragmentShader(std::string filePath) {
    return vsg::ShaderStage::read(VK_SHADER_STAGE_FRAGMENT_BIT, "main", filePath);
}

vsg::ref_ptr<vsg::vec4Array2D> ChVSGSimplePhongIdxMesh::createRGBATexture(
    std::string filePath) {  // read texture image file with stb_image

    vsg::ref_ptr<vsg::vec4Array2D> image;
    filesystem::path testPath(filePath);

    if (testPath.exists() && testPath.is_file()) {
        GetLog() << "texture file '" << filePath << " exists.\n";
        int texWidth = -1, texHeight = -1, texChannels;
        float* pixels = stbi_loadf(filePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        GetLog() << "Breite=" << texWidth << "Höhe=" << texHeight << "\n";
        image = vsg::vec4Array2D::create(texWidth, texHeight, vsg::vec4(0, 0, 0, 0),
                                         vsg::Data::Layout{VK_FORMAT_R32G32B32A32_SFLOAT});
        if (pixels && texWidth > 0 && texHeight > 0) {
            int k = 0;
            for (int j = 0; j < texHeight; j++) {
                for (int i = 0; i < texWidth; i++) {
                    float r = pixels[k++];
                    float g = pixels[k++];
                    float b = pixels[k++];
                    float a = pixels[k++];
                    vsg::vec4 col(r, g, b, a);
                    image->set(i, j, col);
                }
            }
            // release now obsolete pixel buffer
            stbi_image_free(pixels);
        }
    } else {
        GetLog() << "texture file '" << filePath << " not found.\n";
        image = vsg::vec4Array2D::create(2, 2, vsg::vec4(1, 1, 0, 0), vsg::Data::Layout{VK_FORMAT_R32G32B32A32_SFLOAT});
        image->set(0, 0, vsg::vec4(0, 0, 1, 0));
        image->set(1, 1, vsg::vec4(0, 0, 1, 0));
    }
    return image;
}

vsg::ref_ptr<vsg::Node> ChVSGSimplePhongIdxMesh::createVSGNode(DrawMode drawMode) {
    auto subgraph = vsg::StateGroup::create();
    subgraph->setValue("bodyPtr", m_bodyPtr);
    subgraph->setValue("assetPtr", m_assetPtr);
    subgraph->setValue("transform", m_transform);
    switch (drawMode) {
        case DrawMode::Phong: {
            // set up search paths to SPIRV shaders and textures
            vsg::ref_ptr<vsg::ShaderStage> vertexShader =
                readVertexShader(GetChronoDataFile("vsg/shaders/vert_SimplePhong.spv"));
            vsg::ref_ptr<vsg::ShaderStage> fragmentShader =
                readFragmentShader(GetChronoDataFile("vsg/shaders/frag_SimplePhong.spv"));
            if (!vertexShader || !fragmentShader) {
                std::cout << "Could not create shaders." << std::endl;
                return {};
            }

            auto uniformValue = vsg::vec3Value::create(m_objectColor);
            auto uniformBuffer = vsg::DescriptorBuffer::create(uniformValue, 0);
            vsg::DescriptorSetLayoutBindings lightSettingsDescriptorBindings{
                //{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                 VK_SHADER_STAGE_FRAGMENT_BIT,  // we only need it in the fragment shader program
                 nullptr},  // { binding, descriptorTpe, descriptorCount, stageFlags, pImmutableSamplers}
            };

            auto lightSettingsDescriptorSetLayout = vsg::DescriptorSetLayout::create(lightSettingsDescriptorBindings);

            auto uniformDscriptorSet =
                vsg::DescriptorSet::create(lightSettingsDescriptorSetLayout, vsg::Descriptors{uniformBuffer});

            vsg::DescriptorSetLayoutBindings descriptorBindings{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr}  // { binding, descriptorTpe, descriptorCount, stageFlags, pImmutableSamplers}
            };

            auto descriptorSetLayout = vsg::DescriptorSetLayout::create(descriptorBindings);
            vsg::DescriptorSetLayouts descriptorSetLayouts{descriptorSetLayout};

            vsg::PushConstantRanges pushConstantRanges{
                {VK_SHADER_STAGE_VERTEX_BIT, 0, 128}  // projection view, and model matrices, actual push constant calls
                                                      // autoatically provided by the VSG's DispatchTraversal
            };

            auto pipelineLayout = vsg::PipelineLayout::create(
                vsg::DescriptorSetLayouts{descriptorSetLayout, lightSettingsDescriptorSetLayout}, pushConstantRanges);
            auto uniformBindDescriptorSet =
                vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, uniformDscriptorSet);

            vsg::VertexInputState::Bindings vertexBindingsDescriptions{
                VkVertexInputBindingDescription{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},  // vertex data
                VkVertexInputBindingDescription{1, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},  // normal data
                VkVertexInputBindingDescription{2, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},  // normal data
            };

            vsg::VertexInputState::Attributes vertexAttributeDescriptions{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},  // vertex data
                VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},  // normal data
                VkVertexInputAttributeDescription{2, 2, VK_FORMAT_R32G32B32_SFLOAT, 0},  // normal data
            };

            vsg::GraphicsPipelineStates pipelineStates{
                vsg::VertexInputState::create(vertexBindingsDescriptions, vertexAttributeDescriptions),
                vsg::InputAssemblyState::create(),
                vsg::RasterizationState::create(),
                vsg::MultisampleState::create(),
                vsg::ColorBlendState::create(),
                vsg::DepthStencilState::create()};

            auto graphicsPipeline = vsg::GraphicsPipeline::create(
                pipelineLayout, vsg::ShaderStages{vertexShader, fragmentShader}, pipelineStates);
            auto bindGraphicsPipeline = vsg::BindGraphicsPipeline::create(graphicsPipeline);

            // create StateGroup as the root of the scene/command graph to hold the GraphicsProgram, and binding of
            // Descriptors to decorate the whole graph
            subgraph->add(bindGraphicsPipeline);
            // subgraph->add(bindDescriptorSets);
            subgraph->addChild(m_transform);

            // setup geometry
            auto drawCommands = vsg::Commands::create();
            drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{m_vertices, m_normals, m_colors}));
            drawCommands->addChild(vsg::BindIndexBuffer::create(m_indices));
            drawCommands->addChild(vsg::DrawIndexed::create(m_indices->size(), 1, 0, 0, 0));

            // add drawCommands to m_transform
            m_transform->addChild(uniformBindDescriptorSet);
            m_transform->addChild(drawCommands);

        } break;
        case DrawMode::Wireframe: {
            // set up search paths to SPIRV shaders and textures
            vsg::ref_ptr<vsg::ShaderStage> vertexShader =
                readVertexShader(GetChronoDataFile("vsg/shaders/vert_Wireframe.spv"));
            vsg::ref_ptr<vsg::ShaderStage> fragmentShader =
                readFragmentShader(GetChronoDataFile("vsg/shaders/frag_Wireframe.spv"));
            if (!vertexShader || !fragmentShader) {
                std::cout << "Could not create shaders." << std::endl;
                return {};
            }

            auto uniformValue = vsg::vec3Value::create(m_lightPosition);
            auto uniformBuffer = vsg::DescriptorBuffer::create(uniformValue, 0);
            vsg::DescriptorSetLayoutBindings lightSettingsDescriptorBindings{
                //{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_VERTEX_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                 VK_SHADER_STAGE_FRAGMENT_BIT,  // we only need it in the fragment shader program
                 nullptr}  // { binding, descriptorTpe, descriptorCount, stageFlags, pImmutableSamplers}
            };

            auto lightSettingsDescriptorSetLayout = vsg::DescriptorSetLayout::create(lightSettingsDescriptorBindings);

            auto uniformDscriptorSet =
                vsg::DescriptorSet::create(lightSettingsDescriptorSetLayout, vsg::Descriptors{uniformBuffer});

            vsg::DescriptorSetLayoutBindings descriptorBindings{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr}  // { binding, descriptorTpe, descriptorCount, stageFlags, pImmutableSamplers}
            };

            auto descriptorSetLayout = vsg::DescriptorSetLayout::create(descriptorBindings);
            vsg::DescriptorSetLayouts descriptorSetLayouts{descriptorSetLayout};

            vsg::PushConstantRanges pushConstantRanges{
                {VK_SHADER_STAGE_VERTEX_BIT, 0, 128}  // projection view, and model matrices, actual push constant calls
                                                      // autoatically provided by the VSG's DispatchTraversal
            };

            auto pipelineLayout = vsg::PipelineLayout::create(
                vsg::DescriptorSetLayouts{descriptorSetLayout, lightSettingsDescriptorSetLayout}, pushConstantRanges);

            vsg::VertexInputState::Bindings vertexBindingsDescriptions{
                VkVertexInputBindingDescription{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX},  // vertex data
            };

            vsg::VertexInputState::Attributes vertexAttributeDescriptions{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},  // vertex data
                VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},  // normal data
            };

            auto rasterizationState = vsg::RasterizationState::create();
            rasterizationState->polygonMode = VK_POLYGON_MODE_LINE;
            vsg::GraphicsPipelineStates pipelineStates{
                vsg::VertexInputState::create(vertexBindingsDescriptions, vertexAttributeDescriptions),
                vsg::InputAssemblyState::create(),
                rasterizationState,
                vsg::MultisampleState::create(),
                vsg::ColorBlendState::create(),
                vsg::DepthStencilState::create()};

            auto graphicsPipeline = vsg::GraphicsPipeline::create(
                pipelineLayout, vsg::ShaderStages{vertexShader, fragmentShader}, pipelineStates);
            auto bindGraphicsPipeline = vsg::BindGraphicsPipeline::create(graphicsPipeline);

            // create StateGroup as the root of the scene/command graph to hold the GraphicsProgram, and binding of
            // Descriptors to decorate the whole graph
            subgraph->add(bindGraphicsPipeline);
            // subgraph->add(bindDescriptorSets);
            subgraph->addChild(m_transform);

            // setup geometry
            auto drawCommands = vsg::Commands::create();
            drawCommands->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{m_vertices}));
            drawCommands->addChild(vsg::BindIndexBuffer::create(m_indices));
            drawCommands->addChild(vsg::DrawIndexed::create(m_indices->size(), 1, 0, 0, 0));

            // add drawCommands to transform
            m_transform->addChild(drawCommands);

        } break;
    }

    return subgraph;
}