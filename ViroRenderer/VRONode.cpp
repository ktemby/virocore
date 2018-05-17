//
//  VRONode.cpp
//  ViroRenderer
//
//  Created by Raj Advani on 11/15/15.
//  Copyright © 2015 Viro Media. All rights reserved.
//

#include "VRONode.h"
#include "VROGeometry.h"
#include "VROLight.h"
#include "VROAnimation.h"
#include "VROTransaction.h"
#include "VROAnimationVector3f.h"
#include "VROAnimationFloat.h"
#include "VROAnimationQuaternion.h"
#include "VROAction.h"
#include "VROLog.h"
#include "VROHitTestResult.h"
#include "VROAllocationTracker.h"
#include "VROGeometrySource.h"
#include "VROGeometryElement.h"
#include "VROByteBuffer.h"
#include "VROConstraint.h"
#include "VROStringUtil.h"
#include "VROPortal.h"
#include "VROMaterial.h"
#include "VROPhysicsBody.h"
#include "VROParticleEmitter.h"
#include "VROScene.h"
#include "VROAnimationChain.h"
#include "VROExecutableAnimation.h"
#include "VROExecutableNodeAnimation.h"
#include "VROTransformDelegate.h"
#include "VROInstancedUBO.h"

// Opacity below which a node is considered hidden
static const float kHiddenOpacityThreshold = 0.02;

// Set to false to disable visibility testing
static const bool kEnableVisibilityFrustumTest = true;

// Set to true to debut the sort order
bool kDebugSortOrder = false;
int  kDebugSortOrderFrameFrequency = 60;
static int sDebugSortIndex = 0;
const std::string kDefaultNodeTag = "undefined";

// Note: if you change the initial value below, make that sNullNodeID
// in EventDelegate_JNI.cpp still represents a value that this will
// never vend.
std::atomic<int> sUniqueIDGenerator(0);

#pragma mark - Initialization

VRONode::VRONode() : VROThreadRestricted(VROThreadName::Renderer),
    _uniqueID(sUniqueIDGenerator++),
    _type(VRONodeType::Normal),
    _visible(false),
    _lastVisitedRenderingFrame(-1),
    _scale({1.0, 1.0, 1.0}),
    _euler({0, 0, 0}),
    _renderingOrder(0),
    _hidden(false),
    _opacityFromHiddenFlag(1.0),
    _opacity(1.0),
    _computedOpacity(1.0),
    _selectable(true),
    _highAccuracyGaze(false),
    _hierarchicalRendering(false),
    _lightReceivingBitMask(1),
    _shadowCastingBitMask(1),
    _ignoreEventHandling(false),
    _dragType(VRODragType::FixedDistance),
    _dragPlanePoint({0,0,0}),
    _dragPlaneNormal({0,0,0}),
    _dragMaxDistance(10),
    _lastComputedTransform(VROMatrix4f()) {
    ALLOCATION_TRACKER_ADD(Nodes, 1);
}

VRONode::VRONode(const VRONode &node) : VROThreadRestricted(VROThreadName::Renderer),
    _uniqueID(sUniqueIDGenerator++),
    _type(node._type),
    _visible(false),
    _lastVisitedRenderingFrame(-1),
    _geometry(node._geometry),
    _lights(node._lights),
    _sounds(node._sounds),
    _scale(node._scale),
    _position(node._position),
    _rotation(node._rotation),
    _euler(node._euler),
    _renderingOrder(node._renderingOrder),
    _hidden(node._hidden),
    _opacityFromHiddenFlag(node._opacityFromHiddenFlag),
    _opacity(node._opacity),
    _selectable(node._selectable),
    _highAccuracyGaze(node._highAccuracyGaze),
    _hierarchicalRendering(node._hierarchicalRendering),
    _lightReceivingBitMask(node._lightReceivingBitMask),
    _shadowCastingBitMask(node._shadowCastingBitMask),
    _ignoreEventHandling(node._ignoreEventHandling),
    _dragType(node._dragType),
    _dragPlanePoint(node._dragPlanePoint),
    _dragPlaneNormal(node._dragPlaneNormal),
    _dragMaxDistance(node._dragMaxDistance),
    _lastComputedTransform(VROMatrix4f()) {
        
    ALLOCATION_TRACKER_ADD(Nodes, 1);
}

VRONode::~VRONode() {
    ALLOCATION_TRACKER_SUB(Nodes, 1);
}

void VRONode::deleteGL() {
    if (_geometry) {
        _geometry->deleteGL();
    }
    for (std::shared_ptr<VRONode> &subnode : _subnodes) {
        subnode->deleteGL();
    }
}

std::shared_ptr<VRONode> VRONode::clone() {
    std::shared_ptr<VRONode> node = std::make_shared<VRONode>(*this);
    for (std::shared_ptr<VRONode> &subnode : _subnodes) {
        node->addChildNode(subnode->clone());
    }
    
    return node;
}

#pragma mark - Rendering

void VRONode::render(int elementIndex,
                     std::shared_ptr<VROMaterial> &material,
                     const VRORenderContext &context,
                     std::shared_ptr<VRODriver> &driver) {
    passert_thread(__func__);
    
    if (_geometry && _computedOpacity > kHiddenOpacityThreshold) {
        _geometry->render(elementIndex, material,
                          _computedTransform, _computedInverseTransposeTransform, _computedOpacity,
                          context, driver);
    }
}

void VRONode::render(const VRORenderContext &context, std::shared_ptr<VRODriver> &driver) {
    if (_geometry && _computedOpacity > kHiddenOpacityThreshold) {
        for (int i = 0; i < _geometry->getGeometryElements().size(); i++) {
            std::shared_ptr<VROMaterial> &material = _geometry->getMaterialForElement(i);
            if (!material->bindShader(_computedLightsHash, _computedLights, context, driver)) {
                continue;
            }
            material->bindProperties(driver);

            // We render the material if at least one of the following is true:
            //
            // 1. There are lights in the scene that haven't been culled (if there are no lights, then
            //    nothing will be visible! Or,
            // 2. The material is Constant. Constant materials do not need light to be visible. Or,
            // 3. The material is PBR, and we have an active lighting environment. Lighting environments
            //    provide ambient light for PBR materials
            if (!_computedLights.empty() ||
                 material->getLightingModel() == VROLightingModel::Constant ||
                (material->getLightingModel() == VROLightingModel::PhysicallyBased && context.getIrradianceMap() != nullptr)) {


                render(i, material, context, driver);
            }
        }
    }
    
    for (std::shared_ptr<VRONode> &child : _subnodes) {
        child->render(context, driver);
    }
}

void VRONode::renderSilhouettes(std::shared_ptr<VROMaterial> &material,
                                VROSilhouetteMode mode, std::function<bool(const VRONode&)> filter,
                                const VRORenderContext &context, std::shared_ptr<VRODriver> &driver) {
    if (_geometry && _computedOpacity > kHiddenOpacityThreshold) {
        if (!filter || filter(*this)) {
            if (mode == VROSilhouetteMode::Flat) {
                _geometry->renderSilhouette(_computedTransform, material, context, driver);
            }
            else {
                for (int i = 0; i < _geometry->getGeometryElements().size(); i++) {
                    std::shared_ptr<VROTexture> texture = _geometry->getMaterialForElement(i)->getDiffuse().getTexture();
                    if (material->getDiffuse().swapTexture(texture)) {
                        if (!material->bindShader(0, {}, context, driver)) {
                            continue;
                        }
                        material->bindProperties(driver);
                    }
                    _geometry->renderSilhouetteTextured(i, _computedTransform, material, context, driver);
                }
            }
        }
    }
    
    for (std::shared_ptr<VRONode> &child : _subnodes) {
        child->renderSilhouettes(material, mode, filter, context, driver);
    }
}

void VRONode::recomputeUmbrellaBoundingBox() {
    VROMatrix4f parentTransform;
    VROMatrix4f parentRotation;
    
    std::shared_ptr<VRONode> parent = getParentNode();
    if (parent) {
        parentTransform = parent->getComputedTransform();
        parentRotation = parent->getComputedRotation();
    }
    
    // Trigger a computeTransform pass to update the node's bounding boxes and as well as its
    // child's node transforms recursively.
    computeTransforms(parentTransform, parentRotation);
    _umbrellaBoundingBox = VROBoundingBox(_computedPosition.x, _computedPosition.x, _computedPosition.y,
                                          _computedPosition.y, _computedPosition.z, _computedPosition.z);
    computeUmbrellaBounds(&_umbrellaBoundingBox);
}

#pragma mark - Sorting and Transforms

void VRONode::resetDebugSortIndex() {
    sDebugSortIndex = 0;
}

void VRONode::collectLights(std::vector<std::shared_ptr<VROLight>> *outLights) {
    for (std::shared_ptr<VROLight> &light : _lights) {
        light->setTransformedPosition(_computedTransform.multiply(light->getPosition()));
        light->setTransformedDirection(_computedRotation.multiply(light->getDirection()));
        outLights->push_back(light);
    }
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->collectLights(outLights);
    }
}

void VRONode::updateSortKeys(uint32_t depth,
                             VRORenderParameters &params,
                             std::shared_ptr<VRORenderMetadata> &metadata,
                             const VRORenderContext &context,
                             std::shared_ptr<VRODriver> &driver) {
    passert_thread(__func__);
    processActions();

    /*
     If a node is not visible, that means none of its children are visible
     either (we use the umbrella bounding box for visibility tests), so we do
     not have to recurse down.
     */
    if (!_visible) {
        return;
    }

    std::stack<float> &opacities = params.opacities;
    std::vector<std::shared_ptr<VROLight>> &lights = params.lights;
    std::stack<int> &hierarchyDepths = params.hierarchyDepths;
    std::stack<float> &distancesFromCamera = params.distancesFromCamera;

    /*
     Compute specific parameters for this node.
     */
    _computedInverseTransposeTransform = _computedTransform.invert().transpose();
    _computedOpacity = opacities.top() * _opacity * _opacityFromHiddenFlag;
    opacities.push(_computedOpacity);
    
    _computedLights.clear();
    for (std::shared_ptr<VROLight> &light : lights) {
        if ((light->getInfluenceBitMask() & _lightReceivingBitMask) != 0) {

            // Ambient and Directional lights do not attenuate so do not cull them here
            if (light->getType() == VROLightType::Ambient ||
                light->getType() == VROLightType::Directional ||
                getBoundingBox().getDistanceToPoint(light->getTransformedPosition()) < light->getAttenuationEndDistance()) {
                _computedLights.push_back(light);
            }
        }
    }
    _computedLightsHash = VROLight::hashLights(_computedLights);

    /*
     This node uses hierarchical rendering if its flag is set, or if its parent
     used hierarchical rendering.
     */
    int hierarchyDepth = 0;
    int parentHierarchyDepth = hierarchyDepths.top();
    float parentDistanceFromCamera = distancesFromCamera.top();
    
    bool isParentHierarchical = (parentHierarchyDepth >= 0);
    bool isHierarchical = _hierarchicalRendering || isParentHierarchical;
    bool isTopOfHierarchy = _hierarchicalRendering && !isParentHierarchical;
    
    int hierarchyId = 0;
    
    // Distance to camera tracks the min distance between this node's bounding box to
    // the camera, for sort order
    float distanceFromCamera = 0;
    
    // The furthest distance from camera tracks the max distance between this node's
    // bounding box to the camera, for FCP computation
    float furthestDistanceFromCamera = 0;
    
    if (isHierarchical) {
        hierarchyDepth = parentHierarchyDepth + 1;
        hierarchyDepths.push(hierarchyDepth);
        
        if (isTopOfHierarchy) {
            hierarchyId = ++params.hierarchyId;
        }
        else {
            hierarchyId = params.hierarchyId;

            // All children of a hierarchy share the same distance from the camera.
            // This ensures the sort remains stable.
            distanceFromCamera = parentDistanceFromCamera;
        }
    }
    else {
        hierarchyDepths.push(-1);
    }
    
    /*
     Compute the sort key for this node's geometry elements.
     */
    if (_geometry) {
        if (!isHierarchical || isTopOfHierarchy) {
            distanceFromCamera = _computedBoundingBox.getCenter().distance(context.getCamera().getPosition());
            
            // TODO Using the bounding box may be preferred but currently leads to more
            //      artifacts
            // distanceFromCamera = _computedBoundingBox.getDistanceToPoint(context.getCamera().getPosition());
            
            furthestDistanceFromCamera = getBoundingBox().getFurthestDistanceToPoint(context.getCamera().getPosition());
        }
        _geometry->updateSortKeys(this, hierarchyId, hierarchyDepth, _computedLightsHash, _computedLights, _computedOpacity,
                                  distanceFromCamera, context.getZFar(), metadata, context, driver);
        
        if (kDebugSortOrder && context.getFrame() % kDebugSortOrderFrameFrequency == 0) {
            pinfo("   [%d] Pushed node with position [%f, %f, %f], rendering order %d, hierarchy depth %d (actual depth %d), distance to camera %f, hierarchy ID %d, lights %d",
                  sDebugSortIndex, _computedPosition.x, _computedPosition.y, _computedPosition.z, _renderingOrder, hierarchyDepth, depth, distanceFromCamera, hierarchyId, _computedLightsHash);
            _geometry->setName(VROStringUtil::toString(sDebugSortIndex));
        }
    }
    else if (kDebugSortOrder && context.getFrame() % kDebugSortOrderFrameFrequency == 0) {
        pinfo("   [%d] Ignored empty node with position [%f, %f, %f] hierarchy depth %d, distance to camera %f, actual depth %d, hierarchy ID %d",
              sDebugSortIndex, _computedPosition.x, _computedPosition.y, _computedPosition.z,
              hierarchyDepth, 0.0, depth, hierarchyId);
    }

    distancesFromCamera.push(distanceFromCamera);
    params.furthestDistanceFromCamera = std::max(params.furthestDistanceFromCamera, furthestDistanceFromCamera);
    sDebugSortIndex++;
    
    /*
     Move down the tree.
     */
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->updateSortKeys(depth + 1, params, metadata, context, driver);
    }
    
    opacities.pop();
    hierarchyDepths.pop();
    distancesFromCamera.pop();
}

void VRONode::getSortKeysForVisibleNodes(std::vector<VROSortKey> *outKeys) {
    passert_thread(__func__);
    
    // Add the geometry of this node, if available
    if (_visible && _geometry && getType() == VRONodeType::Normal) {
        _geometry->getSortKeys(outKeys);
    }
    
    // Search down the scene graph. If a child is a portal or portal frame,
    // stop the search.
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        if (childNode->getType() == VRONodeType::Normal) {
            childNode->getSortKeysForVisibleNodes(outKeys);
        }
    }
}

void VRONode::computeTransforms(VROMatrix4f parentTransform, VROMatrix4f parentRotation) {
    passert_thread(__func__);
    
    /*
     Compute the transform for this node.
     */
    doComputeTransform(parentTransform);

    /*
     Compute the rotation for this node.
     */
    _computedRotation = parentRotation.multiply(_rotation.getMatrix());

    // Apply the computed transform for spatial sounds, if any.
    for (std::shared_ptr<VROSound> &sound : _sounds) {
        sound->setTransformedPosition(_computedTransform.multiply(sound->getPosition()));
    }

    /*
     Move down the tree.
     */
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->computeTransforms(_computedTransform, _computedRotation);
    }
}

void VRONode::doComputeTransform(VROMatrix4f parentTransform) {
    /*
     Compute the world transform for this node. The full formula is:
     _computedTransform = parentTransform * T * Rpiv * R * Rpiv -1 * Spiv * S * Spiv-1
     */
    _computedTransform.toIdentity();
    
    /*
     Scale.
     */
    if (_scalePivot) {
        VROMatrix4f scale;
        scale.scale(_scale.x, _scale.y, _scale.z);
        _computedTransform = *_scalePivot * scale * *_scalePivotInverse;
    }
    else {
        _computedTransform.scale(_scale.x, _scale.y, _scale.z);
    }
    
    /*
     Rotation.
     */
    if (_rotationPivot) {
        _computedTransform = *_rotationPivotInverse * _computedTransform;
    }
    _computedTransform = _rotation.getMatrix() * _computedTransform;
    if (_rotationPivot) {
        _computedTransform = *_rotationPivot * _computedTransform;
    }
    
    /*
     Translation.
     */
    VROMatrix4f translate;
    translate.translate(_position.x, _position.y, _position.z);
    _computedTransform = translate * _computedTransform;

    _computedTransform = parentTransform * _computedTransform;
    _computedPosition = { _computedTransform[12], _computedTransform[13], _computedTransform[14] };
    if (_geometry) {
        _computedBoundingBox = _geometry->getBoundingBox().transform(_computedTransform);
    } else {
        // If there is no geometry, then the bounding box should be updated to be a 0 size box at the node's position.
        _computedBoundingBox.set(_computedPosition.x, _computedPosition.x, _computedPosition.y,
                                 _computedPosition.y, _computedPosition.z, _computedPosition.z);
    }
}

void VRONode::applyConstraints(const VRORenderContext &context, VROMatrix4f parentTransform,
                               bool parentUpdated) {
    
    bool updated = false;
    
    /*
     If a parent's _computedTransform was updated by constraints, we have to recompute
     the transform for this node as well.
     */
    if (parentUpdated) {
        doComputeTransform(parentTransform);
        updated = true;
    }
    
    /*
     Compute constraints for this node. Do not update _computedRotation as it isn't
     necessary after the afterConstraints() phase.
     */
    for (const std::shared_ptr<VROConstraint> &constraint : _constraints) {
        VROMatrix4f billboardRotation = constraint->getTransform(context, _computedTransform);
        
        // To apply the billboard rotation, translate the object to the origin, apply
        // the rotation, then translate back to its previously computed position
        _computedTransform.translate(_computedPosition.scale(-1));
        _computedTransform = billboardRotation.multiply(_computedTransform);
        _computedTransform.translate(_computedPosition);
        
        updated = true;
    }

    /*
     Move down the tree.
     */
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->applyConstraints(context, _computedTransform, updated);
    }
}

void VRONode::setWorldTransform(VROVector3f finalPosition, VROQuaternion finalRotation){
    // Create a final compute transform representing the desired, final world position and rotation.
    VROVector3f worldScale = getComputedTransform().extractScale();
    VROMatrix4f finalComputedTransform;
    finalComputedTransform.toIdentity();
    finalComputedTransform.scale(worldScale.x, worldScale.y, worldScale.z);
    finalComputedTransform = finalRotation.getMatrix() * finalComputedTransform;
    finalComputedTransform.translate(finalPosition);

    // Calculate local transformations needed to achieve the desired final compute transform
    // by applying: Parent_Trans_INV * FinalCompute = Local_Trans
    VROMatrix4f parentTransform = getParentNode()->getComputedTransform();
    VROMatrix4f currentTransform = parentTransform.invert() * finalComputedTransform;

    _scale = currentTransform.extractScale();
    _position = currentTransform.extractTranslation();
    _rotation = currentTransform.extractRotation(_scale);

    if (getParentNode() == nullptr){
        return;
    }
    // Trigger a computeTransform pass to update the node's bounding boxes and as well as its
    // child's node transforms recursively.
    computeTransforms(getParentNode()->getComputedTransform(), getParentNode()->getComputedRotation());
}

#pragma mark - Atomic Transforms

void VRONode::setPositionAtomic(VROVector3f position) {
    _lastPosition.store(position);
    computeTransformsAtomic();
}

void VRONode::setRotationAtomic(VROQuaternion rotation) {
    _lastRotation.store(rotation);
    computeTransformsAtomic();
}

void VRONode::setScaleAtomic(VROVector3f scale) {
    _lastScale.store(scale);
    computeTransformsAtomic();
}

void VRONode::computeTransformsAtomic(){
    VROMatrix4f parentTransform;
    VROMatrix4f parentRotation;

    // Note that retrieving the parent is thread-safe since it's a shared_ptr, which we then
    // lock. However, we can only safely access atomic properties on the parent
    std::shared_ptr<VRONode> parent = getParentNode();
    if (parent) {
        parentTransform = parent->getLastWorldTransform();
        parentRotation = parent->getLastWorldRotation();
    }
    
    // Trigger a computeAtomicTransforms pass to update the node's bounding boxes and as well as its
    // children's node transforms recursively.
    computeTransformsAtomic(parentTransform, parentRotation);

    // TODO VIRO-3692 Currently it is unsafe to compute the umbrella bounding box because the
    //                subnodes cannot be accessed
    //VROVector3f computedPosition = _lastComputedPosition.load();
    //VROBoundingBox umbrellaBoundingBox(computedPosition.x, computedPosition.x, computedPosition.y,
     //                                  computedPosition.y, computedPosition.z, computedPosition.z);
    //computeUmbrellaBounds(&umbrellaBoundingBox);
}

void VRONode::computeTransformsAtomic(VROMatrix4f parentTransform, VROMatrix4f parentRotation) {
    // This is identical to computeTransform, except it operates on any thread, utilizing
    // only atomic fields
    doComputeTransformsAtomic(parentTransform);
    _lastComputedRotation.store(parentRotation.multiply(_lastRotation.load().getMatrix()));

    // TODO VIRO-3692 Currently it is unsafe to recurse this operation down the graph because
    //                subnodes cannot be accessed
    //for (std::shared_ptr<VRONode> &childNode : _subnodes) {
    //    childNode->computeTransformsAtomic(_lastComputedTransform.load(), _lastComputedRotation.load());
    //}
}

// This sets _lastComputedTransform, _lastComputedPosition, _lastRotation, and _lastComputedBoundingBox
void VRONode::doComputeTransformsAtomic(VROMatrix4f parentTransform) {
    // This is identical to doComputeTransform, except it operates on any thread, utilizing
    // only atomic fields
    VROMatrix4f transform;

    // We ignore scale and rotation pivots since these are not supported by ViroCore or ViroReact.
    // When support *is* added, atomic versions of _scalePivot and _rotationPivot will be necessary.
    VROVector3f scale = _lastScale.load();
    transform.scale(scale.x, scale.y, scale.z);
    transform = _lastRotation.load().getMatrix() * transform;

    // Handle translation as per normal
    VROMatrix4f translate;
    VROVector3f position = _lastPosition.load();
    translate.translate(position.x, position.y, position.z);
    transform = translate * transform;
    
    transform = parentTransform * transform;
    VROVector3f computedPosition = { transform[12], transform[13], transform[14] };
    _lastComputedPosition.store(computedPosition);
    
    if (_geometry) {
        _lastComputedBoundingBox.store(_geometry->getLastBoundingBox().transform(transform));
    } else {
        // If there is no geometry, then the bounding box should be updated to be a 0 size box at the node's position.
        _lastComputedBoundingBox.store({ computedPosition.x, computedPosition.x, computedPosition.y,
                                         computedPosition.y, computedPosition.z, computedPosition.z});
    }
    _lastComputedTransform.store(transform);
}

void VRONode::syncAtomicRenderProperties() {
#if VRO_PLATFORM_IOS || VRO_PLATFORM_ANDROID
    _lastComputedTransform.store(_computedTransform);
    _lastComputedPosition.store(_computedPosition);
    _lastComputedRotation.store(_computedRotation);
    _lastPosition.store(_position);
    _lastRotation.store(_rotation);
    _lastScale.store(_scale);
    _lastComputedBoundingBox.store(_computedBoundingBox);
    _lastUmbrellaBoundingBox.store(_umbrellaBoundingBox);
#endif
    
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->syncAtomicRenderProperties();
    }
}

#pragma mark - Visibility

void VRONode::updateVisibility(const VRORenderContext &context) {
    const VROFrustum &frustum = context.getCamera().getFrustum();
    
    // The umbrellaBoundingBox should be positioned at the node's position, not at [0,0,0],
    // because bounding boxes are in world coordinates
    _umbrellaBoundingBox = VROBoundingBox(_computedPosition.x, _computedPosition.x, _computedPosition.y,
                                          _computedPosition.y, _computedPosition.z, _computedPosition.z);
    computeUmbrellaBounds(&_umbrellaBoundingBox);
    
    VROFrustumResult result = frustum.intersectAllOpt(_umbrellaBoundingBox, &_umbrellaBoxMetadata);
    if (result == VROFrustumResult::Inside || !kEnableVisibilityFrustumTest) {
        setVisibilityRecursive(true);
    }
    else if (result == VROFrustumResult::Intersects) {
        _visible = true;
        for (std::shared_ptr<VRONode> &childNode : _subnodes) {
            childNode->updateVisibility(context);
        }
    }
    else {
        setVisibilityRecursive(false);
    }
}

void VRONode::setVisibilityRecursive(bool visible) {
    _visible = visible;
    
    for (std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->setVisibilityRecursive(visible);
    }
}

void VRONode::computeUmbrellaBounds(VROBoundingBox *bounds) const {
    bounds->unionDestructive(getBoundingBox());
    for (const std::shared_ptr<VRONode> &childNode : _subnodes) {
        childNode->computeUmbrellaBounds(bounds);
    }
}

int VRONode::countVisibleNodes() const {
    int count = _visible ? 1 : 0;
    for (const std::shared_ptr<VRONode> &childNode : _subnodes) {
        count += childNode->countVisibleNodes();
    }
    return count;
}

VROVector3f VRONode::getComputedPosition() const {
    return _computedPosition;
}

VROMatrix4f VRONode::getComputedRotation() const {
    return _computedRotation;
}

VROMatrix4f VRONode::getComputedTransform() const {
    return _computedTransform;
}

VROMatrix4f VRONode::getLastWorldTransform() const {
    return _lastComputedTransform.load();
}

VROVector3f VRONode::getLastWorldPosition() const {
    return _lastComputedPosition.load();
}

VROMatrix4f VRONode::getLastWorldRotation() const {
    return _lastComputedRotation.load();
}

VROVector3f VRONode::getLastLocalPosition() const {
    return _lastPosition.load();
}

VROQuaternion VRONode::getLastLocalRotation() const {
    return _lastRotation.load();
}

VROVector3f VRONode::getLastLocalScale() const {
    return _lastScale.load();
}

VROBoundingBox VRONode::getLastUmbrellaBoundingBox() const {
    return _lastUmbrellaBoundingBox.load();
}

#pragma mark - Scene Graph

void VRONode::addChildNode(std::shared_ptr<VRONode> node) {
    passert_thread(__func__);
    passert (node);
    
    _subnodes.push_back(node);
    node->_supernode = std::static_pointer_cast<VRONode>(shared_from_this());
    
    /*
     If this node is attached to a VROScene, cascade and assign that scene to
     all children.
     */
    std::shared_ptr<VROScene> scene = _scene.lock();
    if (scene) {
        node->setScene(scene, true);
    }
}

void VRONode::removeFromParentNode() {
    passert_thread(__func__);
    
    std::shared_ptr<VRONode> supernode = _supernode.lock();
    if (supernode) {
        std::vector<std::shared_ptr<VRONode>> &parentSubnodes = supernode->_subnodes;
        parentSubnodes.erase(
                             std::remove_if(parentSubnodes.begin(), parentSubnodes.end(),
                                            [this](std::shared_ptr<VRONode> node) {
                                                return node.get() == this;
                                            }), parentSubnodes.end());
        _supernode.reset();
    }
    
    /*
     Detach this node and all its children from the scene.
     */
    setScene(nullptr, true);
}

std::vector<std::shared_ptr<VRONode>> VRONode::getChildNodes() const {
    return _subnodes;
}

void VRONode::setScene(std::shared_ptr<VROScene> scene, bool recursive) {
    /*
     When we detach from a scene, remove any physics bodies from that scene's
     physics world.
     */
    std::shared_ptr<VROScene> currentScene = _scene.lock();
    if (currentScene) {
        if (currentScene->hasPhysicsWorld() && _physicsBody) {
            currentScene->getPhysicsWorld()->removePhysicsBody(_physicsBody);
        }
    }
    
    _scene = scene;
    
    /*
     When we attach to a new scene, add the physics body to the scene's physics
     world.
     */
    if (scene && _physicsBody) {
        scene->getPhysicsWorld()->addPhysicsBody(_physicsBody);
    }
     
    if (recursive) {
        for (std::shared_ptr<VRONode> &node : _subnodes) {
            node->setScene(scene, true);
        }
    }
}

void VRONode::removeAllChildren() {
    std::vector<std::shared_ptr<VRONode>> children = _subnodes;
    for (std::shared_ptr<VRONode> &node : children) {
        node->removeFromParentNode();
    }
}

const std::shared_ptr<VROPortal> VRONode::getParentPortal() const {
    const std::shared_ptr<VRONode> parent = _supernode.lock();
    if (!parent) {
        return nullptr;
    }
    
    if (parent->getType() == VRONodeType::Portal) {
        return std::dynamic_pointer_cast<VROPortal>(parent);
    }
    else {
        return parent->getParentPortal();
    }
}

void VRONode::getChildPortals(std::vector<std::shared_ptr<VROPortal>> *outPortals) const {
    for (const std::shared_ptr<VRONode> &childNode : _subnodes) {
        if (childNode->getType() == VRONodeType::Portal) {
            outPortals->push_back(std::dynamic_pointer_cast<VROPortal>(childNode));
        }
        else {
            childNode->getChildPortals(outPortals);
        }
    }
}

#pragma mark - Setters

void VRONode::setRotation(VROQuaternion rotation) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationQuaternion>([](VROAnimatable *const animatable, VROQuaternion r) {
                                                         ((VRONode *)animatable)->_rotation = r;
                                                         ((VRONode *)animatable)->_euler = r.toEuler();
                                                     }, _rotation, rotation));
}

void VRONode::setRotationEuler(VROVector3f euler) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationVector3f>([](VROAnimatable *const animatable, VROVector3f r) {
                                                        ((VRONode *)animatable)->_euler = VROMathNormalizeAngles2PI(r);
                                                        ((VRONode *)animatable)->_rotation = { r.x, r.y, r.z };
                                                     }, _euler, euler));
}

void VRONode::setPosition(VROVector3f position) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationVector3f>([](VROAnimatable *const animatable, VROVector3f p) {
                                                        VRONode *node = ((VRONode *)animatable);
                                                        node->_position = p;
                                                        node->notifyTransformUpdate(false);
                                                   }, _position, position));
}

void VRONode::setScale(VROVector3f scale) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationVector3f>([](VROAnimatable *const animatable, VROVector3f s) {
                                                       ((VRONode *)animatable)->_scale = s;
                                                   }, _scale, scale));
}

void VRONode::setTransformDelegate(std::shared_ptr<VROTransformDelegate> delegate) {
    _transformDelegate = delegate;

    // Refresh the delegate with the latest position data as it is attached.
    notifyTransformUpdate(true);
}

void VRONode::notifyTransformUpdate(bool forced) {
    std::shared_ptr<VROTransformDelegate> delegate = _transformDelegate.lock();
    if (delegate != nullptr){
        delegate->processPositionUpdate(_position, forced);
    }
}

void VRONode::setPositionX(float x) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float p) {
        VRONode *node = ((VRONode *)animatable);
        node->_position.x = p;
        node->notifyTransformUpdate(false);
    }, _position.x, x));
}

void VRONode::setPositionY(float y) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float p) {
        VRONode *node = ((VRONode *)animatable);
        node->_position.y = p;
        node->notifyTransformUpdate(false);
    }, _position.y, y));
}

void VRONode::setPositionZ(float z) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float p) {
        VRONode *node = ((VRONode *)animatable);
        node->_position.z = p;
        node->notifyTransformUpdate(false);
    }, _position.z, z));
}

void VRONode::setScaleX(float x) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float s) {
        ((VRONode *)animatable)->_scale.x = s;
    }, _scale.x, x));
}

void VRONode::setScaleY(float y) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float s) {
        ((VRONode *)animatable)->_scale.y = s;
    }, _scale.y, y));
}

void VRONode::setScaleZ(float z) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float s) {
        ((VRONode *)animatable)->_scale.z = s;
    }, _scale.z, z));
}

void VRONode::setRotationEulerX(float radians) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float r) {
        VROVector3f &euler = ((VRONode *) animatable)->_euler;
        euler.x = VROMathNormalizeAngle2PI(r);
        ((VRONode *)animatable)->_rotation = { euler.x, euler.y, euler.z };
    }, _euler.x, radians));
}

void VRONode::setRotationEulerY(float radians) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float r) {
        VROVector3f &euler = ((VRONode *) animatable)->_euler;
        euler.y = VROMathNormalizeAngle2PI(r);
        ((VRONode *)animatable)->_rotation = { euler.x, euler.y, euler.z };
    }, _euler.y, radians));
}

void VRONode::setRotationEulerZ(float radians) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float r) {
        VROVector3f &euler = ((VRONode *) animatable)->_euler;
        euler.z = VROMathNormalizeAngle2PI(r);
        ((VRONode *)animatable)->_rotation = { euler.x, euler.y, euler.z };
    }, _euler.z, radians));
}

void VRONode::setRotationPivot(VROMatrix4f pivot) {
    passert_thread(__func__);
    _rotationPivot = pivot;
    _rotationPivotInverse = pivot.invert();
}

void VRONode::setScalePivot(VROMatrix4f pivot) {
    passert_thread(__func__);
    _scalePivot = pivot;
    _scalePivotInverse = pivot.invert();
}

void VRONode::setOpacity(float opacity) {
    passert_thread(__func__);
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float s) {
        ((VRONode *)animatable)->_opacity = s;
    }, _opacity, opacity));
}

void VRONode::setHidden(bool hidden) {
    passert_thread(__func__);
    _hidden = hidden;
    
    float opacity = hidden ? 0.0 : 1.0;
    animate(std::make_shared<VROAnimationFloat>([](VROAnimatable *const animatable, float s) {
        ((VRONode *)animatable)->_opacityFromHiddenFlag = s;
    }, _opacityFromHiddenFlag, opacity));
}

void VRONode::setHighAccuracyGaze(bool enabled) {
    passert_thread(__func__);
    _highAccuracyGaze = enabled;
}

#pragma mark - Actions and Animations

void VRONode::processActions() {
    passert_thread(__func__);
    
    std::vector<std::shared_ptr<VROAction>>::iterator it;
    for (it = _actions.begin(); it != _actions.end(); ++it) {
        std::shared_ptr<VROAction> &action = *it;

        action->execute(this);

        if (action->getType() == VROActionType::PerFrame ||
            action->getType() == VROActionType::Timed) {
            
            // Remove the action when it's complete
            if (!action->shouldRepeat()) {
                _actions.erase(it);
                --it;
            }
        }
        else {            
            // Remove the action; it will be re-added (if needed) after the animation
            _actions.erase(it);
            --it;
        }
    }
}

void VRONode::runAction(std::shared_ptr<VROAction> action) {
    passert_thread(__func__);
    _actions.push_back(action);
}

void VRONode::removeAction(std::shared_ptr<VROAction> action) {
    passert_thread(__func__);
    _actions.erase(std::remove_if(_actions.begin(), _actions.end(),
                                  [action](std::shared_ptr<VROAction> candidate) {
                                      return candidate == action;
                                  }), _actions.end());
}

void VRONode::removeAllActions() {
    passert_thread(__func__);
    _actions.clear();
}

void VRONode::addAnimation(std::string key, std::shared_ptr<VROExecutableAnimation> animation) {
    passert_thread(__func__);
    std::shared_ptr<VRONode> shared = std::dynamic_pointer_cast<VRONode>(shared_from_this());
    _animations[key].push_back(std::make_shared<VROExecutableNodeAnimation>(shared, animation));
}

void VRONode::removeAnimation(std::string key) {
    passert_thread(__func__);
    auto kv = _animations.find(key);
    if (kv == _animations.end()) {
        return;
    }
    
    for (std::shared_ptr<VROExecutableAnimation> &animation : kv->second) {
        animation->terminate(false);
    }
    kv->second.clear();
}

std::shared_ptr<VROExecutableAnimation> VRONode::getAnimation(std::string key, bool recursive) {
    std::vector<std::shared_ptr<VROExecutableAnimation>> animations;
    getAnimations(animations, key, recursive);
    
    return std::make_shared<VROAnimationChain>(animations, VROAnimationChainExecution::Parallel);
}

void VRONode::getAnimations(std::vector<std::shared_ptr<VROExecutableAnimation>> &animations,
                            std::string key, bool recursive) {
    auto kv = _animations.find(key);
    if (kv != _animations.end()) {
        animations.insert(animations.end(), kv->second.begin(), kv->second.end());
    }
    
    if (recursive) {
        for (std::shared_ptr<VRONode> &subnode : _subnodes) {
            subnode->getAnimations(animations, key, recursive);
        }
    }
}

std::set<std::string> VRONode::getAnimationKeys(bool recursive) {
    std::set<std::string> animations;
    getAnimationKeys(animations, recursive);
    
    return animations;
}

void VRONode::getAnimationKeys(std::set<std::string> &keys, bool recursive) {
    for (auto kv : _animations) {
        if (!kv.second.empty()) {
            keys.insert(kv.first);
        }
    }
    if (recursive) {
        for (std::shared_ptr<VRONode> &subnode : _subnodes) {
            subnode->getAnimationKeys(keys, recursive);
        }
    }
}

void VRONode::removeAllAnimations() {
    passert_thread(__func__);
    for (auto kv : _animations) {
        for (std::shared_ptr<VROExecutableAnimation> &animation : kv.second) {
            animation->terminate(true);
        }
        kv.second.clear();
    }
    _animations.clear();
}

void VRONode::onAnimationFinished() {
    notifyTransformUpdate(true);

    std::shared_ptr<VROPhysicsBody> body = getPhysicsBody();
    if (body){
        body->refreshBody();
    }
}

#pragma mark - Hit Testing

VROBoundingBox VRONode::getBoundingBox() const {
    if (_geometry && _geometry->getInstancedUBO() != nullptr){
        return _geometry->getInstancedUBO()->getInstancedBoundingBox();
    }
    return _computedBoundingBox;
}

VROBoundingBox VRONode::getUmbrellaBoundingBox() const {
    return _umbrellaBoundingBox;
}

std::vector<VROHitTestResult> VRONode::hitTest(const VROCamera &camera, VROVector3f origin, VROVector3f ray,
                                               bool boundsOnly) {
    passert_thread(__func__);
    std::vector<VROHitTestResult> results;

    VROMatrix4f identity;
    hitTest(camera, origin, ray, boundsOnly, results);
    return results;
}

void VRONode::hitTest(const VROCamera &camera, VROVector3f origin, VROVector3f ray, bool boundsOnly,
                      std::vector<VROHitTestResult> &results) {
    passert_thread(__func__);
    if (!_selectable) {
        return;
    }
    
    VROMatrix4f transform = _computedTransform;
    boundsOnly = boundsOnly && !getHighAccuracyGaze();
    
    if (_geometry && _computedOpacity > kHiddenOpacityThreshold && _visible) {
        VROVector3f intPt;
        if (getBoundingBox().intersectsRay(ray, origin, &intPt)) {
            if (boundsOnly || hitTestGeometry(origin, ray, transform)) {
                results.push_back( {std::static_pointer_cast<VRONode>(shared_from_this()),
                                    intPt,
                                    origin.distance(intPt), false,
                                    camera });
            }
        }
    }
    
    for (std::shared_ptr<VRONode> &subnode : _subnodes) {
        subnode->hitTest(camera, origin, ray, boundsOnly, results);
    }
}

bool VRONode::hitTestGeometry(VROVector3f origin, VROVector3f ray, VROMatrix4f transform) {
    passert_thread(__func__);
    std::shared_ptr<VROGeometrySource> vertexSource = _geometry->getGeometrySourcesForSemantic(VROGeometrySourceSemantic::Vertex).front();
    
    bool hit = false;
    for (std::shared_ptr<VROGeometryElement> element : _geometry->getGeometryElements()) {
         element->processTriangles([&hit, ray, origin, transform](int index, VROTriangle triangle) {
             VROTriangle transformed = triangle.transformByMatrix(transform);
             
             VROVector3f intPt;
             if (transformed.intersectsRay(ray, origin, &intPt)) {
                 hit = true;
                 //TODO Offer a way to break out of here, as optimization
             }
         }, vertexSource);
    }
    
    return hit;
}

#pragma mark - Constraints

void VRONode::addConstraint(std::shared_ptr<VROConstraint> constraint) {
    passert_thread(__func__);
    _constraints.push_back(constraint);
}

void VRONode::removeConstraint(std::shared_ptr<VROConstraint> constraint) {
    passert_thread(__func__);
    _constraints.erase(std::remove_if(_constraints.begin(), _constraints.end(),
                                  [constraint](std::shared_ptr<VROConstraint> candidate) {
                                      return candidate == constraint;
                                  }), _constraints.end());
}

void VRONode::removeAllConstraints() {
    passert_thread(__func__);
    _constraints.clear();
}

#pragma mark - Physics

std::shared_ptr<VROPhysicsBody> VRONode::initPhysicsBody(VROPhysicsBody::VROPhysicsBodyType type, float mass,
                                                         std::shared_ptr<VROPhysicsShape> shape) {
    std::shared_ptr<VRONode> node = std::static_pointer_cast<VRONode>(shared_from_this());
    _physicsBody = std::make_shared<VROPhysicsBody>(node, type, mass, shape);
    
    std::shared_ptr<VROScene> scene = _scene.lock();
    if (scene) {
        scene->getPhysicsWorld()->addPhysicsBody(_physicsBody);
    }
    return _physicsBody;
}

std::shared_ptr<VROPhysicsBody> VRONode::getPhysicsBody() const {
    return _physicsBody;
}

void VRONode::clearPhysicsBody() {
    if (_physicsBody) {
        std::shared_ptr<VROScene> scene = _scene.lock();
        if (scene && scene->hasPhysicsWorld()) {
            scene->getPhysicsWorld()->removePhysicsBody(_physicsBody);
        }
    }
    _physicsBody = nullptr;
}

#pragma mark - Particle Emitters

void VRONode::updateParticles(const VRORenderContext &context) {
    if (_particleEmitter) {
        // Check if the particle emitter's surface has changed
        if (_geometry != _particleEmitter->getParticleSurface()) {
            _geometry = _particleEmitter->getParticleSurface();
        }
        
        // Update the emitter
        _particleEmitter->update(context, _computedTransform);
    }
    
    // Recurse to children
    for (std::shared_ptr<VRONode> &child : _subnodes) {
        child->updateParticles(context);
    }
}

void VRONode::setParticleEmitter(std::shared_ptr<VROParticleEmitter> emitter) {
    passert_thread(__func__);
    _particleEmitter = emitter;
    _geometry = emitter->getParticleSurface();
    setIgnoreEventHandling(true);
}

void VRONode::removeParticleEmitter() {
    passert_thread(__func__);
    _particleEmitter.reset();
    _geometry.reset();
    setIgnoreEventHandling(false);
}

std::shared_ptr<VROParticleEmitter> VRONode::getParticleEmitter() const {
    return _particleEmitter;
}
