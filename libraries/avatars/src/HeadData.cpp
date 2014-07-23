//
//  HeadData.cpp
//  libraries/avatars/src
//
//  Created by Stephen Birarda on 5/20/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <glm/gtx/quaternion.hpp>

#include <OctreeConstants.h>

#include "AvatarData.h"
#include "HeadData.h"

#include "../animation/src/FacialAnimationData.h"

HeadData::HeadData(AvatarData* owningAvatar) :
    _baseYaw(0.0f),
    _basePitch(0.0f),
    _baseRoll(0.0f),
    _leanSideways(0.0f),
    _leanForward(0.0f),
    _lookAtPosition(0.0f, 0.0f, 0.0f),
    _audioLoudness(0.0f),
    _isFaceshiftConnected(false),
    _leftEyeBlink(0.0f),
    _rightEyeBlink(0.0f),
    _averageLoudness(0.0f),
    _browAudioLift(0.0f),
    _facialAnimationData(new FacialAnimationData),
    _owningAvatar(owningAvatar)
{
    
}

glm::quat HeadData::getOrientation() const {
    return _owningAvatar->getOrientation() * glm::quat(glm::radians(glm::vec3(_basePitch, _baseYaw, _baseRoll)));
}

void HeadData::setOrientation(const glm::quat& orientation) {
    // rotate body about vertical axis
    glm::quat bodyOrientation = _owningAvatar->getOrientation();
    glm::vec3 newFront = glm::inverse(bodyOrientation) * (orientation * IDENTITY_FRONT);
    bodyOrientation = bodyOrientation * glm::angleAxis(atan2f(-newFront.x, -newFront.z), glm::vec3(0.0f, 1.0f, 0.0f));
    _owningAvatar->setOrientation(bodyOrientation);
    
    // the rest goes to the head
    glm::vec3 eulers = glm::degrees(safeEulerAngles(glm::inverse(bodyOrientation) * orientation));
    _basePitch = eulers.x;
    _baseYaw = eulers.y;
    _baseRoll = eulers.z;
}

void HeadData::setLeftBlink(float val) {
    _facialAnimationData->setLeftBlink(val);
}

void HeadData::setRightBlink(float val) {
    _facialAnimationData->setRightBlink(val);
}

void HeadData::setLeftEyeOpen(float val) {
    _facialAnimationData->setLeftEyeOpen(val);
}

void HeadData::setRightEyeOpen(float val) {
    _facialAnimationData->setRightEyeOpen(val);
}

void HeadData::setBrowDownLeft(float val) {
    _facialAnimationData->setBrowDownLeft(val);
}

void HeadData::setBrowDownRight(float val) {
    _facialAnimationData->setBrowDownRight(val);
}

void HeadData::setBrowUpCenter(float val) {
    _facialAnimationData->setBrowUpCenter(val);
}

void HeadData::setBrowUpLeft(float val) {
    _facialAnimationData->setBrowUpLeft(val);
}

void HeadData::setBrowUpRight(float val) {
    _facialAnimationData->setBrowUpRight(val);
}

void HeadData::setMouthSize(float val) {
    _facialAnimationData->setMouthSize(val);
}

void HeadData::setMouthSmileLeft(float val) {
    _facialAnimationData->setLeftBlink(val);
}

void HeadData::setMouthSmileRight(float val) {
    _facialAnimationData->setMouthSmileRight(val);
}


void HeadData::addYaw(float yaw) {
    setBaseYaw(_baseYaw + yaw);
}

void HeadData::addPitch(float pitch) {
    setBasePitch(_basePitch + pitch);
}

void HeadData::addRoll(float roll) {
    setBaseRoll(_baseRoll + roll);
}

const QVector<float>& HeadData::getBlendshapeCoefficients() const { 
    return _facialAnimationData->_blendshapeCoefficients;
}