/**
 * @file ActorContract.h
 * @brief Test-only observations shared by the public actor consumer and its DLL.
 */

#pragma once

#include <Spark/GameObject.h>

#include <cstddef>

struct ActorObservations
{
    unsigned updates = 0;
    unsigned hits = 0;
    unsigned worldHits = 0;
    unsigned renders = 0;
    unsigned destructions = 0;
    HRESULT meshResult = S_OK;
};

using CreateActor = GameObject* (*)(unsigned, ActorObservations*);
using ReadActorMatrix = float (*)(const GameObject*, unsigned, unsigned);
using ReadActorLayout = std::size_t (*)(unsigned, bool);
using VerifyMeshValues = bool (*)();
