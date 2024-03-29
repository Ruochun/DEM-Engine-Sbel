deme::clumpComponentOffset_t myCompOffset = granData->clumpComponentOffset[sphereID];
if (myCompOffset != deme::RESERVED_CLUMP_COMPONENT_OFFSET) {
    myRelPosX = CDRelPosX[myCompOffset];
    myRelPosY = CDRelPosY[myCompOffset];
    myRelPosZ = CDRelPosZ[myCompOffset];
    myRadius = Radii[myCompOffset];
} else {
    // Look for my components in global memory
    deme::clumpComponentOffsetExt_t myCompOffsetExt = granData->clumpComponentOffsetExt[sphereID];
    myRelPosX = granData->relPosSphereX[myCompOffsetExt];
    myRelPosY = granData->relPosSphereY[myCompOffsetExt];
    myRelPosZ = granData->relPosSphereZ[myCompOffsetExt];
    myRadius = granData->radiiSphere[myCompOffsetExt];
}
