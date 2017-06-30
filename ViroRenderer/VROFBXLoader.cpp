//
//  VROFBXLoader.cpp
//  ViroRenderer
//
//  Created by Raj Advani on 5/1/17.
//  Copyright © 2017 Viro Media. All rights reserved.
//

#include "VROFBXLoader.h"
#include "VRONode.h"
#include "VROPlatformUtil.h"
#include "VROGeometry.h"
#include "VROData.h"
#include "VROStringUtil.h"
#include "VROModelIOUtil.h"
#include "VROSkinner.h"
#include "VROSkeleton.h"
#include "VROBone.h"
#include "VROSkeletalAnimation.h"
#include "VROBoneUBO.h"
#include "Nodes.pb.h"

VROGeometrySourceSemantic convert(viro::Node_Geometry_Source_Semantic semantic) {
    switch (semantic) {
        case viro::Node_Geometry_Source_Semantic_Vertex:
            return VROGeometrySourceSemantic::Vertex;
        case viro::Node_Geometry_Source_Semantic_Normal:
            return VROGeometrySourceSemantic::Normal;
        case viro::Node_Geometry_Source_Semantic_Color:
            return VROGeometrySourceSemantic::Color;
        case viro::Node_Geometry_Source_Semantic_Texcoord:
            return VROGeometrySourceSemantic::Texcoord;
        case viro::Node_Geometry_Source_Semantic_Tangent:
            return VROGeometrySourceSemantic::Tangent;
        case viro::Node_Geometry_Source_Semantic_VertexCrease:
            return VROGeometrySourceSemantic::VertexCrease;
        case viro::Node_Geometry_Source_Semantic_EdgeCrease:
            return VROGeometrySourceSemantic::EdgeCrease;
        case viro::Node_Geometry_Source_Semantic_BoneWeights:
            return VROGeometrySourceSemantic::BoneWeights;
        case viro::Node_Geometry_Source_Semantic_BoneIndices:
            return VROGeometrySourceSemantic::BoneIndices;
        default:
            pabort();
    }
}

VROGeometryPrimitiveType convert(viro::Node_Geometry_Element_Primitive primitive) {
    switch (primitive) {
        case viro::Node_Geometry_Element_Primitive_Triangle:
            return VROGeometryPrimitiveType::Triangle;
        case viro::Node_Geometry_Element_Primitive_TriangleStrip:
            return VROGeometryPrimitiveType::TriangleStrip;
        case viro::Node_Geometry_Element_Primitive_Line:
            return VROGeometryPrimitiveType::Line;
        case viro::Node_Geometry_Element_Primitive_Point:
            return VROGeometryPrimitiveType::Point;
        default:
            pabort();
    }
}

VROLightingModel convert(viro::Node_Geometry_Material_LightingModel lightingModel) {
    switch (lightingModel) {
        case viro::Node_Geometry_Material_LightingModel_Constant:
            return VROLightingModel::Constant;
        case viro::Node_Geometry_Material_LightingModel_Lambert:
            return VROLightingModel::Lambert;
        case viro::Node_Geometry_Material_LightingModel_Blinn:
            return VROLightingModel::Blinn;
        case viro::Node_Geometry_Material_LightingModel_Phong:
            return VROLightingModel::Phong;
        default:
            pabort();
    }
}

VROWrapMode convert(viro::Node_Geometry_Material_Visual_WrapMode wrapMode) {
    switch (wrapMode) {
        case viro::Node_Geometry_Material_Visual_WrapMode_Clamp:
            return VROWrapMode::Clamp;
        case viro::Node_Geometry_Material_Visual_WrapMode_ClampToBorder:
            return VROWrapMode::ClampToBorder;
        case viro::Node_Geometry_Material_Visual_WrapMode_Mirror:
            return VROWrapMode::Mirror;
        case viro::Node_Geometry_Material_Visual_WrapMode_Repeat:
            return VROWrapMode::Repeat;
        default:
            pabort();
    }
}

VROFilterMode convert(viro::Node_Geometry_Material_Visual_FilterMode filterMode) {
    switch (filterMode) {
        case viro::Node_Geometry_Material_Visual_FilterMode_Linear:
            return VROFilterMode::Linear;
        case viro::Node_Geometry_Material_Visual_FilterMode_Nearest:
            return VROFilterMode::Nearest;
        case viro::Node_Geometry_Material_Visual_FilterMode_None:
            return VROFilterMode::None;
        default:
            pabort();
    }
}

void setTextureProperties(const viro::Node::Geometry::Material::Visual &pb, std::shared_ptr<VROTexture> &texture) {
    texture->setMinificationFilter(convert(pb.minification_filter()));
    texture->setMagnificationFilter(convert(pb.magnification_filter()));
    texture->setMipFilter(convert(pb.mip_filter()));
    texture->setWrapS(convert(pb.wrap_mode_s()));
    texture->setWrapT(convert(pb.wrap_mode_t()));
}

std::shared_ptr<VRONode> VROFBXLoader::loadFBXFromURL(std::string url, std::string baseURL,
                                                      bool async, std::function<void(std::shared_ptr<VRONode>, bool)> onFinish) {
    std::shared_ptr<VRONode> node = std::make_shared<VRONode>();
    
    if (async) {
        VROPlatformDispatchAsyncBackground([url, baseURL, node, onFinish] {
            bool isTemp = false;
            bool success = false;
            std::string file = VROPlatformDownloadURLToFile(url, &isTemp, &success);
            
            std::shared_ptr<VRONode> fbxNode;
            if (success) {
                fbxNode = loadFBX(file, baseURL, true, nullptr);
            }
            if (isTemp) {
                VROPlatformDeleteFile(file);
            }
            
            VROPlatformDispatchAsyncRenderer([node, fbxNode, onFinish] {
                injectFBX(fbxNode, node, onFinish);
            });
        });
    }
    else {
        bool isTemp = false;
        bool success = false;
        std::string file = VROPlatformDownloadURLToFile(url, &isTemp, &success);
        
        std::shared_ptr<VRONode> fbxNode;
        if (success) {
            fbxNode = loadFBX(file, baseURL, true, nullptr);
        }
        if (isTemp) {
            VROPlatformDeleteFile(file);
        }
        
        injectFBX(fbxNode, node, onFinish);
    }
    
    return node;
}

std::shared_ptr<VRONode> VROFBXLoader::loadFBXFromFile(std::string file, std::string baseDir,
                                                       bool async, std::function<void(std::shared_ptr<VRONode>, bool)> onFinish) {
    
    std::shared_ptr<VRONode> node = std::make_shared<VRONode>();
    
    if (async) {
        VROPlatformDispatchAsyncBackground([file, baseDir, node, onFinish] {
            std::shared_ptr<VRONode> fbxNode = loadFBX(file, baseDir, false, nullptr);
            VROPlatformDispatchAsyncRenderer([node, fbxNode, onFinish] {
                injectFBX(fbxNode, node, onFinish);
            });
        });
    }
    else {
        std::shared_ptr<VRONode> fbxNode = loadFBX(file, baseDir, false, nullptr);
        injectFBX(fbxNode, node, onFinish);
    }
    
    return node;
}

std::shared_ptr<VRONode> VROFBXLoader::loadFBXFromFileWithResources(std::string file, std::map<std::string, std::string> resourceMap,
                                                                    bool async, std::function<void(std::shared_ptr<VRONode>, bool)> onFinish) {
    
    std::shared_ptr<VRONode> node = std::make_shared<VRONode>();
    
    if (async) {
        VROPlatformDispatchAsyncBackground([file, resourceMap, node, onFinish] {
            std::shared_ptr<VRONode> fbxNode = loadFBX(file, "", false, &resourceMap);
            VROPlatformDispatchAsyncRenderer([node, fbxNode, onFinish] {
                injectFBX(fbxNode, node, onFinish);
            });
        });
    }
    else {
        std::shared_ptr<VRONode> fbxNode = loadFBX(file, "", false, &resourceMap);
        injectFBX(fbxNode, node, onFinish);
    }
    
    return node;
}

void VROFBXLoader::injectFBX(std::shared_ptr<VRONode> fbxNode, std::shared_ptr<VRONode> node,
                             std::function<void(std::shared_ptr<VRONode> node, bool success)> onFinish) {
    
    if (fbxNode) {
        // The top-level fbxNode is a dummy; all of the data is stored in the children, so we
        // simply transfer those children over to the destination node
        for (std::shared_ptr<VRONode> child : fbxNode->getSubnodes()) {
            node->addChildNode(child);
        }
        if (onFinish) {
            onFinish(node, true);
        }
    }
    else {
        if (onFinish) {
            onFinish(node, false);
        }
    }
}

std::shared_ptr<VRONode> VROFBXLoader::loadFBX(std::string file, std::string base, bool isBaseURL,
                                               const std::map<std::string, std::string> *resourceMap) {
    
    std::map<std::string, std::shared_ptr<VROTexture>> textureCache;

    pinfo("Loading FBX from file %s", file.c_str());
    std::string data_pb = VROPlatformLoadFileAsString(file);
    
    viro::Node node_pb;
    if (!node_pb.ParseFromString(data_pb)) {
        pinfo("Failed to parse FBX protobuf");
        return {};
    }
    
    pinfo("Read FBX protobuf");
    
    // The root node contains the skeleton, if any
    std::shared_ptr<VROSkeleton> skeleton;
    if (node_pb.has_skeleton()) {
        skeleton = loadFBXSkeleton(node_pb.skeleton());
    }
    
    // The outer node of the protobuf has no mesh data, it contains
    // metadata (like the skeleton) and holds the root nodes of the
    // FBX mesh. We use our outer VRONode for the same purpose, to
    // contain the root nodes of the FBX file
    std::shared_ptr<VRONode> rootNode = std::make_shared<VRONode>();
    rootNode->setThreadRestrictionEnabled(false);
    for (int i = 0; i < node_pb.subnode_size(); i++) {
        std::shared_ptr<VRONode> node = loadFBXNode(node_pb.subnode(i), skeleton, base, isBaseURL,
                                                    resourceMap, textureCache);
        rootNode->addChildNode(node);
    }
    
    return rootNode;
}

std::shared_ptr<VRONode> VROFBXLoader::loadFBXNode(const viro::Node &node_pb,
                                                   std::shared_ptr<VROSkeleton> skeleton,
                                                   std::string base, bool isBaseURL,
                                                   const std::map<std::string, std::string> *resourceMap,
                                                   std::map<std::string, std::shared_ptr<VROTexture>> &textureCache) {
    
    pinfo("Loading node [%s]", node_pb.name().c_str());
    
    std::shared_ptr<VRONode> node = std::make_shared<VRONode>();
    node->setThreadRestrictionEnabled(false);
    node->setPosition({ node_pb.position(0), node_pb.position(1), node_pb.position(2) });
    node->setScale({ node_pb.scale(0), node_pb.scale(1), node_pb.scale(2) });
    node->setRotation({ (float) degrees_to_radians(node_pb.rotation(0)),
                        (float) degrees_to_radians(node_pb.rotation(1)),
                        (float) degrees_to_radians(node_pb.rotation(2)) });
    node->setRenderingOrder(node_pb.rendering_order());
    node->setOpacity(node_pb.opacity());
    
    if (node_pb.has_geometry()) {
        const viro::Node_Geometry &geo_pb = node_pb.geometry();
        std::shared_ptr<VROGeometry> geo = loadFBXGeometry(geo_pb, base, isBaseURL, resourceMap, textureCache);
        
        if (geo_pb.has_skin() && skeleton) {
            geo->setSkinner(loadFBXSkinner(geo_pb.skin(), skeleton));
            
            bool hasScaling = false;
            for (int i = 0; i < node_pb.skeletal_animation_size(); i++) {
                const viro::Node::SkeletalAnimation &animation_pb = node_pb.skeletal_animation(i);
                if (animation_pb.has_scaling()) {
                    hasScaling = true;
                }
                
                std::shared_ptr<VROSkeletalAnimation> animation = loadFBXSkeletalAnimation(animation_pb, skeleton);
                if (animation->getName().empty()) {
                    animation->setName("fbx_animation_" + VROStringUtil::toString(i));
                }
                
                node->addAnimation(animation->getName(), animation);
                pinfo("   Added animation [%s]", animation->getName().c_str());
            }
            
            if (hasScaling) {
                pinfo("   At least 1 animation has scaling: using DQ+S modifier");
            }
            
            for (const std::shared_ptr<VROMaterial> &material : geo->getMaterials()) {
                material->addShaderModifier(VROBoneUBO::createSkinningShaderModifier(hasScaling));
            }
        }
        node->setGeometry(geo);
    }
    
    for (int i = 0; i < node_pb.subnode_size(); i++) {
        std::shared_ptr<VRONode> subnode = loadFBXNode(node_pb.subnode(i), skeleton, base, isBaseURL,
                                                       resourceMap, textureCache);
        node->addChildNode(subnode);
    }
    
    return node;
}

std::shared_ptr<VROGeometry> VROFBXLoader::loadFBXGeometry(const viro::Node_Geometry &geo_pb,
                                                           std::string base, bool isBaseURL,
                                                           const std::map<std::string, std::string> *resourceMap,
                                                           std::map<std::string, std::shared_ptr<VROTexture>> &textureCache) {
    std::shared_ptr<VROData> varData = std::make_shared<VROData>(geo_pb.data().c_str(), geo_pb.data().length());
    
    std::vector<std::shared_ptr<VROGeometrySource>> sources;
    for (int i = 0; i < geo_pb.source_size(); i++) {
        const viro::Node::Geometry::Source &source_pb = geo_pb.source(i);
        std::shared_ptr<VROGeometrySource> source = std::make_shared<VROGeometrySource>(varData,
                                                                                        convert(source_pb.semantic()),
                                                                                        source_pb.vertex_count(),
                                                                                        source_pb.float_components(),
                                                                                        source_pb.components_per_vertex(),
                                                                                        source_pb.bytes_per_component(),
                                                                                        source_pb.data_offset(),
                                                                                        source_pb.data_stride());
        sources.push_back(source);
    }
    
    std::vector<std::shared_ptr<VROGeometryElement>> elements;
    for (int i = 0; i < geo_pb.element_size(); i++) {
        const viro::Node::Geometry::Element &element_pb = geo_pb.element(i);
        
        std::shared_ptr<VROData> data = std::make_shared<VROData>(element_pb.data().c_str(), element_pb.data().length());
        std::shared_ptr<VROGeometryElement> element = std::make_shared<VROGeometryElement>(data,
                                                                                           convert(element_pb.primitive()),
                                                                                           element_pb.primitive_count(),
                                                                                           element_pb.bytes_per_index());
        elements.push_back(element);
    }
    
    std::shared_ptr<VROGeometry> geo = std::make_shared<VROGeometry>(sources, elements);
    geo->setName(geo_pb.name());
    
    std::vector<std::shared_ptr<VROMaterial>> materials;
    for (int i = 0; i < geo_pb.material_size(); i++) {
        const viro::Node::Geometry::Material &material_pb = geo_pb.material(i);
        
        std::shared_ptr<VROMaterial> material = std::make_shared<VROMaterial>();
        material->setName(material_pb.name());
        material->setShininess(material_pb.shininess());
        material->setFresnelExponent(material_pb.fresnel_exponent());
        material->setTransparency(material_pb.transparency());
        material->setLightingModel(convert(material_pb.lighting_model()));
        material->setReadsFromDepthBuffer(true);
        material->setWritesToDepthBuffer(true);
        
        if (material_pb.has_diffuse()) {
            const viro::Node::Geometry::Material::Visual &diffuse_pb = material_pb.diffuse();
            VROMaterialVisual &diffuse = material->getDiffuse();
            
            if (diffuse_pb.color_size() >= 2) {
                diffuse.setColor({ diffuse_pb.color(0), diffuse_pb.color(1), diffuse_pb.color(2), 1.0 });
            }
            diffuse.setIntensity(diffuse_pb.intensity());
            
            if (!diffuse_pb.texture().empty()) {
                std::shared_ptr<VROTexture> texture = VROModelIOUtil::loadTexture(diffuse_pb.texture(), base, isBaseURL, resourceMap, textureCache);
                if (texture) {
                    diffuse.setTexture(texture);
                    setTextureProperties(diffuse_pb, texture);
                }
                else {
                    pinfo("FBX failed to load diffuse texture [%s]", diffuse_pb.texture().c_str());
                }
            }
        }
        if (material_pb.has_specular()) {
            const viro::Node::Geometry::Material::Visual &specular_pb = material_pb.specular();
            VROMaterialVisual &specular = material->getSpecular();
            
            specular.setIntensity(specular_pb.intensity());
            
            if (!specular_pb.texture().empty()) {
                std::shared_ptr<VROTexture> texture = VROModelIOUtil::loadTexture(specular_pb.texture(), base, isBaseURL, resourceMap, textureCache);
                if (texture) {
                    specular.setTexture(texture);
                    setTextureProperties(specular_pb, texture);
                }
                else {
                    pinfo("FBX failed to load specular texture [%s]", specular_pb.texture().c_str());
                }
            }
        }
        if (material_pb.has_normal()) {
            const viro::Node::Geometry::Material::Visual &normal_pb = material_pb.normal();
            VROMaterialVisual &normal = material->getNormal();
            
            normal.setIntensity(normal_pb.intensity());
            
            if (!normal_pb.texture().empty()) {
                std::shared_ptr<VROTexture> texture = VROModelIOUtil::loadTexture(normal_pb.texture(), base, isBaseURL, resourceMap, textureCache);
                if (texture) {
                    normal.setTexture(texture);
                    setTextureProperties(normal_pb, texture);
                }
                else {
                    pinfo("FBX failed to load normal texture [%s]", normal_pb.texture().c_str());
                }
            }
        }
        
        materials.push_back(material);
    }
    geo->setMaterials(materials);
    
    VROBoundingBox bounds = geo->getBoundingBox();
    pinfo("   Bounds x(%f %f)", bounds.getMinX(), bounds.getMaxX());
    pinfo("          y(%f %f)", bounds.getMinY(), bounds.getMaxY());
    pinfo("          z(%f %f)", bounds.getMinZ(), bounds.getMaxZ());
    
    return geo;
}

std::shared_ptr<VROSkeleton> VROFBXLoader::loadFBXSkeleton(const viro::Node_Skeleton &skeleton_pb) {
    std::vector<std::shared_ptr<VROBone>> bones;
    for (int i = 0; i < skeleton_pb.bone_size(); i++) {
        std::shared_ptr<VROBone> bone = std::make_shared<VROBone>(skeleton_pb.bone(i).parent_index());
        bones.push_back(bone);
    }
    
    return std::make_shared<VROSkeleton>(bones);
}

std::unique_ptr<VROSkinner> VROFBXLoader::loadFBXSkinner(const viro::Node_Geometry_Skin &skin_pb,
                                                         std::shared_ptr<VROSkeleton> skeleton) {
    
    float geometryBindMtx[16];
    for (int j = 0; j < 16; j++) {
        geometryBindMtx[j] = skin_pb.geometry_bind_transform().value(j);
    }
    VROMatrix4f geometryBindTransform(geometryBindMtx);
    
    std::vector<VROMatrix4f> bindTransforms;
    for (int i = 0; i < skin_pb.bind_transform_size(); i++) {
        
        if (skin_pb.bind_transform(i).value_size() != 16) {
            // Push identity if we don't have a bind transform for this bone
            bindTransforms.push_back({});
        }
        else {
            float mtx[16];
            for (int j = 0; j < 16; j++) {
                mtx[j] = skin_pb.bind_transform(i).value(j);
            }
            bindTransforms.push_back({ mtx });
        }
    }
    
    const viro::Node::Geometry::Source &bone_indices_pb = skin_pb.bone_indices();
    std::shared_ptr<VROData> boneIndicesData = std::make_shared<VROData>(bone_indices_pb.data().c_str(), bone_indices_pb.data().length());
    std::shared_ptr<VROGeometrySource> boneIndices = std::make_shared<VROGeometrySource>(boneIndicesData,
                                                                                         convert(bone_indices_pb.semantic()),
                                                                                         bone_indices_pb.vertex_count(),
                                                                                         bone_indices_pb.float_components(),
                                                                                         bone_indices_pb.components_per_vertex(),
                                                                                         bone_indices_pb.bytes_per_component(),
                                                                                         bone_indices_pb.data_offset(),
                                                                                         bone_indices_pb.data_stride());
    
    const viro::Node::Geometry::Source &bone_weights_pb = skin_pb.bone_weights();
    std::shared_ptr<VROData> boneWeightsData = std::make_shared<VROData>(bone_weights_pb.data().c_str(), bone_weights_pb.data().length());
    std::shared_ptr<VROGeometrySource> boneWeights = std::make_shared<VROGeometrySource>(boneWeightsData,
                                                                                         convert(bone_weights_pb.semantic()),
                                                                                         bone_weights_pb.vertex_count(),
                                                                                         bone_weights_pb.float_components(),
                                                                                         bone_weights_pb.components_per_vertex(),
                                                                                         bone_weights_pb.bytes_per_component(),
                                                                                         bone_weights_pb.data_offset(),
                                                                                         bone_weights_pb.data_stride());
    
    
    
    return std::unique_ptr<VROSkinner>(new VROSkinner(skeleton, geometryBindTransform, bindTransforms, boneIndices, boneWeights));
}

std::shared_ptr<VROSkeletalAnimation> VROFBXLoader::loadFBXSkeletalAnimation(const viro::Node_SkeletalAnimation &animation_pb,
                                                                             std::shared_ptr<VROSkeleton> skeleton) {
    
    std::vector<std::unique_ptr<VROSkeletalAnimationFrame>> frames;
    for (int f = 0; f < animation_pb.frame_size(); f++) {
        const viro::Node::SkeletalAnimation::Frame &frame_pb = animation_pb.frame(f);
        
        std::unique_ptr<VROSkeletalAnimationFrame> frame = std::unique_ptr<VROSkeletalAnimationFrame>(new VROSkeletalAnimationFrame());
        frame->time = frame_pb.time();
        
        passert (frame_pb.bone_index_size() == frame_pb.transform_size());
        for (int b = 0; b < frame_pb.bone_index_size(); b++) {
            frame->boneIndices.push_back(frame_pb.bone_index(b));
            
            float mtx[16];
            for (int i = 0; i < 16; i++) {
                mtx[i] = frame_pb.transform(b).value(i);
            }
            frame->boneTransforms.push_back({ mtx });
        }
        
        frames.push_back(std::move(frame));
    }
    
    float duration = animation_pb.duration();
    
    std::shared_ptr<VROSkeletalAnimation> animation = std::make_shared<VROSkeletalAnimation>(skeleton, frames, duration);
    animation->setName(animation_pb.name());
    
    return animation;
}
