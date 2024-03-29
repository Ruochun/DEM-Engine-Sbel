// DEM bin--sphere relations-related custom kernels
#include <DEM/Defines.h>
#include <kernel/DEMHelperKernels.cu>

// If clump templates are jitified, they will be below
_clumpTemplateDefs_;
// Definitions of analytical entites are below
_analyticalEntityDefs_;
// Family mask, _nFamilyMaskEntries_ elements are in this array
// __constant__ __device__ bool familyMasks[] = {_familyMasks_};

__global__ void getNumberOfBinsEachSphereTouches(deme::DEMSimParams* simParams,
                                                 deme::DEMDataKT* granData,
                                                 deme::binsSphereTouches_t* numBinsSphereTouches,
                                                 deme::objID_t* numAnalGeoSphereTouches) {
    deme::bodyID_t sphereID = blockIdx.x * blockDim.x + threadIdx.x;
    if (sphereID < simParams->nSpheresGM) {
        // Register sphere--analytical geometry contacts
        deme::objID_t contact_count = 0;
        // Sphere's family ID
        unsigned int sphFamilyNum;
        double myPosX, myPosY, myPosZ;
        double myRadius;
        {
            // My sphere voxel ID and my relPos
            deme::bodyID_t myOwnerID = granData->ownerClumpBody[sphereID];
            sphFamilyNum = granData->familyID[myOwnerID];
            float myRelPosX, myRelPosY, myRelPosZ;
            double ownerX, ownerY, ownerZ;

            // Get my component offset info from either jitified arrays or global memory
            // Outputs myRelPosXYZ, myRadius (in CD kernels, radius needs to be expanded)
            // Use an input named exactly `sphereID' which is the id of this sphere component
            {
                _componentAcqStrat_;
                myRadius += simParams->beta;
            }

            voxelIDToPosition<double, deme::voxelID_t, deme::subVoxelPos_t>(
                ownerX, ownerY, ownerZ, granData->voxelID[myOwnerID], granData->locX[myOwnerID],
                granData->locY[myOwnerID], granData->locZ[myOwnerID], _nvXp2_, _nvYp2_, _voxelSize_, _l_);
            const float myOriQw = granData->oriQw[myOwnerID];
            const float myOriQx = granData->oriQx[myOwnerID];
            const float myOriQy = granData->oriQy[myOwnerID];
            const float myOriQz = granData->oriQz[myOwnerID];
            applyOriQToVector3<float, deme::oriQ_t>(myRelPosX, myRelPosY, myRelPosZ, myOriQw, myOriQx, myOriQy,
                                                    myOriQz);
            // The bin number that I live in (with fractions)?
            myPosX = ownerX + (double)myRelPosX;
            myPosY = ownerY + (double)myRelPosY;
            myPosZ = ownerZ + (double)myRelPosZ;
            double myBinX = myPosX / simParams->binSize;
            double myBinY = myPosY / simParams->binSize;
            double myBinZ = myPosZ / simParams->binSize;
            // How many bins my radius spans (with fractions)?
            double myRadiusSpan = myRadius / simParams->binSize;
            // printf("myRadius: %f\n", myRadiusSpan);
            // Now, figure out how many bins I touch in each direction
            deme::binsSphereTouches_t numX =
                (unsigned int)(myBinX + myRadiusSpan) - (unsigned int)(myBinX - myRadiusSpan) + 1;
            deme::binsSphereTouches_t numY =
                (unsigned int)(myBinY + myRadiusSpan) - (unsigned int)(myBinY - myRadiusSpan) + 1;
            deme::binsSphereTouches_t numZ =
                (unsigned int)(myBinZ + myRadiusSpan) - (unsigned int)(myBinZ - myRadiusSpan) + 1;
            // TODO: Add an error message if numX * numY * numZ > MAX(binsSphereTouches_t)

            // Write the number of bins this sphere touches back to the global array
            numBinsSphereTouches[sphereID] = numX * numY * numZ;
            // printf("This sp takes num of bins: %u\n", numX * numY * numZ);
        }

        // Each sphere entity should also check if it overlaps with an analytical boundary-type geometry
        for (deme::objID_t objB = 0; objB < simParams->nAnalGM; objB++) {
            deme::contact_t contact_type;
            deme::bodyID_t objBOwner = objOwner[objB];
            // Grab family number from memory (not jitified: b/c family number can change frequently in a sim)
            unsigned int objFamilyNum = granData->familyID[objBOwner];
            unsigned int maskMatID =
                locateMaskPair<unsigned int>((unsigned int)sphFamilyNum, (unsigned int)objFamilyNum);
            // If marked no contact, skip ths iteration
            if (granData->familyMasks[maskMatID] != deme::DONT_PREVENT_CONTACT) {
                continue;
            }
            double ownerX, ownerY, ownerZ;
            voxelIDToPosition<double, deme::voxelID_t, deme::subVoxelPos_t>(
                ownerX, ownerY, ownerZ, granData->voxelID[objBOwner], granData->locX[objBOwner],
                granData->locY[objBOwner], granData->locZ[objBOwner], _nvXp2_, _nvYp2_, _voxelSize_, _l_);
            const float ownerOriQw = granData->oriQw[objBOwner];
            const float ownerOriQx = granData->oriQx[objBOwner];
            const float ownerOriQy = granData->oriQy[objBOwner];
            const float ownerOriQz = granData->oriQz[objBOwner];
            float objBRelPosX = objRelPosX[objB];
            float objBRelPosY = objRelPosY[objB];
            float objBRelPosZ = objRelPosZ[objB];
            float objBRotX = objRotX[objB];
            float objBRotY = objRotY[objB];
            float objBRotZ = objRotZ[objB];
            applyOriQToVector3<float, deme::oriQ_t>(objBRelPosX, objBRelPosY, objBRelPosZ, ownerOriQw, ownerOriQx,
                                                    ownerOriQy, ownerOriQz);
            applyOriQToVector3<float, deme::oriQ_t>(objBRotX, objBRotY, objBRotZ, ownerOriQw, ownerOriQx, ownerOriQy,
                                                    ownerOriQz);
            double objBPosX = ownerX + (double)objBRelPosX;
            double objBPosY = ownerY + (double)objBRelPosY;
            double objBPosZ = ownerZ + (double)objBRelPosZ;
            contact_type = checkSphereEntityOverlap<double>(
                myPosX, myPosY, myPosZ, myRadius, objType[objB], objBPosX, objBPosY, objBPosZ, objBRotX, objBRotY,
                objBRotZ, objSize1[objB], objSize2[objB], objSize3[objB], objNormal[objB], simParams->beta);

            if (contact_type) {
                contact_count++;
            }
        }
        numAnalGeoSphereTouches[sphereID] = contact_count;
    }
}

__global__ void populateBinSphereTouchingPairs(deme::DEMSimParams* simParams,
                                               deme::DEMDataKT* granData,
                                               deme::binSphereTouchPairs_t* numBinsSphereTouchesScan,
                                               deme::binSphereTouchPairs_t* numAnalGeoSphereTouchesScan,
                                               deme::binID_t* binIDsEachSphereTouches,
                                               deme::bodyID_t* sphereIDsEachBinTouches,
                                               deme::bodyID_t* idGeoA,
                                               deme::bodyID_t* idGeoB,
                                               deme::contact_t* contactType) {
    deme::bodyID_t sphereID = blockIdx.x * blockDim.x + threadIdx.x;
    if (sphereID < simParams->nSpheresGM) {
        double myPosX, myPosY, myPosZ;
        double myRadius;
        unsigned int sphFamilyNum;
        deme::binSphereTouchPairs_t mySphereGeoReportOffset = numAnalGeoSphereTouchesScan[sphereID];
        {
            // My sphere voxel ID and my relPos
            deme::bodyID_t myOwnerID = granData->ownerClumpBody[sphereID];
            sphFamilyNum = granData->familyID[myOwnerID];
            float myRelPosX, myRelPosY, myRelPosZ;
            double ownerX, ownerY, ownerZ;

            // Get my component offset info from either jitified arrays or global memory
            // Outputs myRelPosXYZ, myRadius (in CD kernels, radius needs to be expanded)
            // Use an input named exactly `sphereID' which is the id of this sphere component
            {
                _componentAcqStrat_;
                myRadius += simParams->beta;
            }

            // Get the offset of my spot where I should start writing back to the global bin--sphere pair registration
            // array
            deme::binSphereTouchPairs_t myReportOffset = numBinsSphereTouchesScan[sphereID];
            voxelIDToPosition<double, deme::voxelID_t, deme::subVoxelPos_t>(
                ownerX, ownerY, ownerZ, granData->voxelID[myOwnerID], granData->locX[myOwnerID],
                granData->locY[myOwnerID], granData->locZ[myOwnerID], _nvXp2_, _nvYp2_, _voxelSize_, _l_);
            const float myOriQw = granData->oriQw[myOwnerID];
            const float myOriQx = granData->oriQx[myOwnerID];
            const float myOriQy = granData->oriQy[myOwnerID];
            const float myOriQz = granData->oriQz[myOwnerID];
            applyOriQToVector3<float, deme::oriQ_t>(myRelPosX, myRelPosY, myRelPosZ, myOriQw, myOriQx, myOriQy,
                                                    myOriQz);
            // The bin number that I live in (with fractions)?
            myPosX = ownerX + (double)myRelPosX;
            myPosY = ownerY + (double)myRelPosY;
            myPosZ = ownerZ + (double)myRelPosZ;
            double myBinX = myPosX / simParams->binSize;
            double myBinY = myPosY / simParams->binSize;
            double myBinZ = myPosZ / simParams->binSize;
            // How many bins my radius spans (with fractions)?
            double myRadiusSpan = myRadius / simParams->binSize;
            // Now, write the IDs of those bins that I touch, back to the global memory
            deme::binID_t thisBinID;
            for (unsigned int k = (unsigned int)(myBinZ - myRadiusSpan); k <= (unsigned int)(myBinZ + myRadiusSpan);
                 k++) {
                for (unsigned int j = (unsigned int)(myBinY - myRadiusSpan); j <= (unsigned int)(myBinY + myRadiusSpan);
                     j++) {
                    for (unsigned int i = (unsigned int)(myBinX - myRadiusSpan);
                         i <= (unsigned int)(myBinX + myRadiusSpan); i++) {
                        thisBinID = (deme::binID_t)i + (deme::binID_t)j * simParams->nbX +
                                    (deme::binID_t)k * simParams->nbX * simParams->nbY;
                        binIDsEachSphereTouches[myReportOffset] = thisBinID;
                        sphereIDsEachBinTouches[myReportOffset] = sphereID;
                        myReportOffset++;
                    }
                }
            }
        }

        // Each sphere entity should also check if it overlaps with an analytical boundary-type geometry
        for (deme::objID_t objB = 0; objB < simParams->nAnalGM; objB++) {
            deme::contact_t contact_type;
            deme::bodyID_t objBOwner = objOwner[objB];
            // Grab family number from memory (not jitified: b/c family number can change frequently in a sim)
            unsigned int objFamilyNum = granData->familyID[objBOwner];
            unsigned int maskMatID = locateMaskPair<unsigned int>(sphFamilyNum, objFamilyNum);
            // If marked no contact, skip ths iteration
            if (granData->familyMasks[maskMatID] != deme::DONT_PREVENT_CONTACT) {
                continue;
            }
            double ownerX, ownerY, ownerZ;
            voxelIDToPosition<double, deme::voxelID_t, deme::subVoxelPos_t>(
                ownerX, ownerY, ownerZ, granData->voxelID[objBOwner], granData->locX[objBOwner],
                granData->locY[objBOwner], granData->locZ[objBOwner], _nvXp2_, _nvYp2_, _voxelSize_, _l_);
            const float ownerOriQw = granData->oriQw[objBOwner];
            const float ownerOriQx = granData->oriQx[objBOwner];
            const float ownerOriQy = granData->oriQy[objBOwner];
            const float ownerOriQz = granData->oriQz[objBOwner];
            float objBRelPosX = objRelPosX[objB];
            float objBRelPosY = objRelPosY[objB];
            float objBRelPosZ = objRelPosZ[objB];
            float objBRotX = objRotX[objB];
            float objBRotY = objRotY[objB];
            float objBRotZ = objRotZ[objB];
            applyOriQToVector3<float, deme::oriQ_t>(objBRelPosX, objBRelPosY, objBRelPosZ, ownerOriQw, ownerOriQx,
                                                    ownerOriQy, ownerOriQz);
            applyOriQToVector3<float, deme::oriQ_t>(objBRotX, objBRotY, objBRotZ, ownerOriQw, ownerOriQx, ownerOriQy,
                                                    ownerOriQz);
            double objBPosX = ownerX + (double)objBRelPosX;
            double objBPosY = ownerY + (double)objBRelPosY;
            double objBPosZ = ownerZ + (double)objBRelPosZ;
            contact_type = checkSphereEntityOverlap<double>(
                myPosX, myPosY, myPosZ, myRadius, objType[objB], objBPosX, objBPosY, objBPosZ, objBRotX, objBRotY,
                objBRotZ, objSize1[objB], objSize2[objB], objSize3[objB], objNormal[objB], simParams->beta);

            if (contact_type) {
                idGeoA[mySphereGeoReportOffset] = sphereID;
                idGeoB[mySphereGeoReportOffset] = (deme::bodyID_t)objB;
                contactType[mySphereGeoReportOffset] = contact_type;
                mySphereGeoReportOffset++;
            }
        }
    }
}
