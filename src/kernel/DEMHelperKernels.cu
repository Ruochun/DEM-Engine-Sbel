// DEM device-side helper kernel collection
//#include <thirdparty/nvidia_helper_math/helper_math.cuh>
#include <DEM/Defines.h>

// I can only include CUDAMathHelpers.cu here and if I do it in other kernel files such as DEMBinSphereKernels.cu too,
// there will be double-load problem where operators are re-defined. Not sure how to resolve it.
#include <kernel/CUDAMathHelpers.cu>

// inline __device__ voxelID_t position2VoxelID

////////////////////////////////////////////////////////////////////////////////
// A few helper functions specific to DEM module
////////////////////////////////////////////////////////////////////////////////

// Sign function
template <typename T1>
inline __device__ int sgn(const T1& val) {
    return (T1(0) < val) - (val < T1(0));
}

template <typename T1>
inline __device__ T1 dot3(const T1& x1, const T1& x2, const T1& x3, const T1& y1, const T1& y2, const T1& y3) {
    return x1 * y1 + x2 * y2 + x3 * y3;
}

// Integer division that rounds towards -infty
template <typename T1, typename T2>
inline __device__ T1 div_floor(const T1& a, const T2& b) {
    T1 res = a / b;
    T1 rem = a % b;
    // Correct division result downwards if up-rounding happened,
    // (for non-zero remainder of sign different than the divisor).
    T1 corr = (rem != 0 && ((rem < 0) != (b < 0)));
    return res - corr;
}

// Modulus that rounds towards -infty
template <typename T1, typename T2>
inline __device__ T1 mod_floor(const T1& a, const T2& b) {
    if (b < 0)  // you can check for b == 0 separately and do what you want
        return -mod_floor(-a, -b);
    T1 ret = a % b;
    if (ret < 0)
        ret += b;
    return ret;
}

// In an upper-triangular (including the diagonal part) matrix, given i and j, this function returns the index of the
// corresponding flatten-ed non-zero entries (col-major like in matlab). This function does not assume i <= j. It is
// used in locating masks that maps the contact between families.
template <typename T1>
inline __device__ T1 locateMaskPair(const T1& i, const T1& j) {
    if (i > j)
        return locateMaskPair(j, i);
    return (1 + j) * j / 2 + i;
}

// Magic function that converts an index of a flatten-ed upper-triangular matrix (EXCLUDING the diagonal) to its
// corresponding i and j. It is ROW-major. It is used to map contact pair numbers in a bin.
template <typename T1>
inline __device__ void recoverCntPair(T1& i, T1& j, const T1& ind, const T1& n) {
    i = n - 2 - (T1)(sqrt((float)(4 * n * (n - 1) - 7 - 8 * ind)) / 2.0 - 0.5);
    j = ind + i + 1 + (n - i) * ((n - i) - 1) / 2 - n * (n - 1) / 2;
}

// Chops a long ID (typically voxelID) into XYZ components
template <typename T1, typename T2>
inline __device__ void IDChopper(T1& X,
                                 T1& Y,
                                 T1& Z,
                                 const T2& ID,
                                 const unsigned char& nvXp2,
                                 const unsigned char& nvYp2) {
    X = ID & (((T1)1 << nvXp2) - 1);  // & operation here equals modulo
    Y = (ID >> nvXp2) & (((T1)1 << nvYp2) - 1);
    Z = (ID) >> (nvXp2 + nvYp2);
}

// Packs XYZ components back to a long ID (typically voxelID)
template <typename T1, typename T2>
inline __device__ void IDPacker(T1& ID,
                                const T2& X,
                                const T2& Y,
                                const T2& Z,
                                const unsigned char& nvXp2,
                                const unsigned char& nvYp2) {
    ID = X;
    ID += Y << nvXp2;
    ID += Z << (nvXp2 + nvYp2);
}

// From a voxelID to (usually double-precision) xyz coordinate
template <typename T1, typename T2, typename T3>
inline __device__ void voxelIDToPosition(T1& X,
                                         T1& Y,
                                         T1& Z,
                                         const T2& ID,
                                         const T3& subPosX,
                                         const T3& subPosY,
                                         const T3& subPosZ,
                                         const unsigned char& nvXp2,
                                         const unsigned char& nvYp2,
                                         const T1& voxelSize,
                                         const T1& l) {
    T2 voxelIDX, voxelIDY, voxelIDZ;
    IDChopper<T2, T2>(voxelIDX, voxelIDY, voxelIDZ, ID, nvXp2, nvYp2);
    X = (T1)voxelIDX * voxelSize + (T1)subPosX * l;
    Y = (T1)voxelIDY * voxelSize + (T1)subPosY * l;
    Z = (T1)voxelIDZ * voxelSize + (T1)subPosZ * l;
}

// From xyz coordinate (usually double-precision) to voxelID
template <typename T1, typename T2, typename T3>
inline __device__ void positionToVoxelID(T1& ID,
                                         T2& subPosX,
                                         T2& subPosY,
                                         T2& subPosZ,
                                         const T3& X,
                                         const T3& Y,
                                         const T3& Z,
                                         const unsigned char& nvXp2,
                                         const unsigned char& nvYp2,
                                         const T3& voxelSize,
                                         const T3& l) {
    deme::voxelID_t voxelNumX = X / voxelSize;
    deme::voxelID_t voxelNumY = Y / voxelSize;
    deme::voxelID_t voxelNumZ = Z / voxelSize;
    subPosX = (X - (T3)voxelNumX * voxelSize) / l;
    subPosY = (Y - (T3)voxelNumY * voxelSize) / l;
    subPosZ = (Z - (T3)voxelNumZ * voxelSize) / l;

    ID = voxelNumX;
    ID += voxelNumY << nvXp2;
    ID += voxelNumZ << (nvXp2 + nvYp2);
}

template <typename T1, typename T2>
inline __device__ void applyOriQToVector3(T1& X, T1& Y, T1& Z, const T2& Qw, const T2& Qx, const T2& Qy, const T2& Qz) {
    T1 oldX = X;
    T1 oldY = Y;
    T1 oldZ = Z;
    X = ((T2)2.0 * (Qw * Qw + Qx * Qx) - (T2)1.0) * oldX + ((T2)2.0 * (Qx * Qy - Qw * Qz)) * oldY +
        ((T2)2.0 * (Qx * Qz + Qw * Qy)) * oldZ;
    Y = ((T2)2.0 * (Qx * Qy + Qw * Qz)) * oldX + ((T2)2.0 * (Qw * Qw + Qy * Qy) - (T2)1.0) * oldY +
        ((T2)2.0 * (Qy * Qz - Qw * Qx)) * oldZ;
    Z = ((T2)2.0 * (Qx * Qz - Qw * Qy)) * oldX + ((T2)2.0 * (Qy * Qz + Qw * Qx)) * oldY +
        ((T2)2.0 * (Qw * Qw + Qz * Qz) - (T2)1.0) * oldZ;
}

template <typename T1>
inline __device__ T1 distSquared(const T1& x1, const T1& y1, const T1& z1, const T1& x2, const T1& y2, const T1& z2) {
    return (x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2);
}

// Normalize a 3-component vector
template <typename T1>
inline __device__ void normalizeVector3(T1& x, T1& y, T1& z) {
    T1 magnitude = sqrt(x * x + y * y + z * z);
    // TODO: Think about whether this is safe
    // if (magnitude < DEME_TINY_FLOAT) {
    //     printf("Caution!\n");
    // }
    x /= magnitude;
    y /= magnitude;
    z /= magnitude;
}

// Hamilton product of 2 quaternions
template <typename T1>
inline __device__ void HamiltonProduct(T1& A,
                                       T1& B,
                                       T1& C,
                                       T1& D,
                                       const T1 a1,
                                       const T1 b1,
                                       const T1 c1,
                                       const T1 d1,
                                       const T1 a2,
                                       const T1 b2,
                                       const T1 c2,
                                       const T1 d2) {
    A = a1 * a2 - b1 * b2 - c1 * c2 - d1 * d2;
    B = a1 * b2 + b1 * a2 + c1 * d2 - d1 * c2;
    C = a1 * c2 - b1 * d2 + c1 * a2 + d1 * b2;
    D = a1 * d2 + b1 * c2 - c1 * b2 + d1 * a2;
}

/**
 * Template arguments:
 *   - T1: the floating point accuracy level for contact point location/penetration depth
 *
 * Basic idea: determines whether 2 spheres intersect and the intersection point coordinates
 *
 */
template <typename T1>
inline __device__ deme::contact_t checkSpheresOverlap(const T1& XA,
                                                      const T1& YA,
                                                      const T1& ZA,
                                                      const T1& radA,
                                                      const T1& XB,
                                                      const T1& YB,
                                                      const T1& ZB,
                                                      const T1& radB,
                                                      T1& CPX,
                                                      T1& CPY,
                                                      T1& CPZ) {
    T1 centerDist2 = distSquared<T1>(XA, YA, ZA, XB, YB, ZB);
    if (centerDist2 > (radA + radB) * (radA + radB)) {
        return deme::NOT_A_CONTACT;
    }
    // If getting this far, then 2 spheres have an intersection, let's calculate the intersection point
    float B2AVecX = XA - XB;
    float B2AVecY = YA - YB;
    float B2AVecZ = ZA - ZB;
    normalizeVector3<float>(B2AVecX, B2AVecY, B2AVecZ);
    T1 halfOverlapDepth = (radA + radB - sqrt(centerDist2)) / (T1)2;
    // From center of B, towards center of A, move a distance of radB, then backtrack a bit, for half the overlap depth
    CPX = XB + (radB - halfOverlapDepth) * B2AVecX;
    CPY = YB + (radB - halfOverlapDepth) * B2AVecY;
    CPZ = ZB + (radB - halfOverlapDepth) * B2AVecZ;
    return deme::SPHERE_SPHERE_CONTACT;
}

/**
 * Template arguments:
 *   - T1: the floating point accuracy level for contact point location/penetration depth
 *   - T2: the floating point accuracy level for the relative position of 2 bodies involved
 *
 * Basic idea: this is another version of checkSpheresOverlap which also gives the penetration length and bodyB's
 * outward contact normal
 *
 */
template <typename T1, typename T2>
inline __device__ deme::contact_t checkSpheresOverlap(const T1& XA,
                                                      const T1& YA,
                                                      const T1& ZA,
                                                      const T1& radA,
                                                      const T1& XB,
                                                      const T1& YB,
                                                      const T1& ZB,
                                                      const T1& radB,
                                                      T1& CPX,
                                                      T1& CPY,
                                                      T1& CPZ,
                                                      T2& normalX,
                                                      T2& normalY,
                                                      T2& normalZ,
                                                      T1& overlapDepth) {
    T1 centerDist2 = distSquared<T1>(XA, YA, ZA, XB, YB, ZB);
    if (centerDist2 > (radA + radB) * (radA + radB)) {
        return deme::NOT_A_CONTACT;
    }
    // If getting this far, then 2 spheres have an intersection, let's calculate the intersection point
    normalX = XA - XB;
    normalY = YA - YB;
    normalZ = ZA - ZB;
    normalizeVector3<T2>(normalX, normalY, normalZ);
    overlapDepth = radA + radB - sqrt(centerDist2);
    // From center of B, towards center of A, move a distance of radB, then backtrack a bit, for half the overlap depth
    CPX = XB + (radB - overlapDepth / (T1)2) * normalX;
    CPY = YB + (radB - overlapDepth / (T1)2) * normalY;
    CPZ = ZB + (radB - overlapDepth / (T1)2) * normalZ;
    return deme::SPHERE_SPHERE_CONTACT;
}

template <typename T1>
inline __device__ T1
getPointBinID(const double& X, const double& Y, const double& Z, const double& binSize, const T1& nbX, const T1& nbY) {
    T1 binIDX = X / binSize;
    T1 binIDY = Y / binSize;
    T1 binIDZ = Z / binSize;
    return binIDX + binIDY * nbX + binIDZ * nbX * nbY;
}

/**
 * Template arguments:
 *   - T1: the floating point accuracy level for the point coordinates
 *
 * Basic idea: calculate a vector that goes from B to A, and pack it as a float3
 *
 */
template <typename T1>
inline __device__ float3 vectorAB(const T1& AX, const T1& AY, const T1& AZ, const T1& BX, const T1& BY, const T1& BZ) {
    return make_float3(AX - BX, AY - BY, AZ - BZ);
}

/**
 * Template arguments:
 *   - T1: the floating point accuracy level for the point coordinate and the frame O coordinate
 *
 * Basic idea: calculate a point's (typically contact point) local coordinate in a specific frame, then return as a
 * float3
 *
 */
template <typename T1>
inline __device__ float3 findLocalCoord(const T1& X,
                                        const T1& Y,
                                        const T1& Z,
                                        const T1& Ox,
                                        const T1& Oy,
                                        const T1& Oz,
                                        const deme::oriQ_t& oriQw,
                                        const deme::oriQ_t& oriQx,
                                        const deme::oriQ_t& oriQy,
                                        const deme::oriQ_t& oriQz) {
    float locX, locY, locZ;
    locX = X - Ox;
    locY = Y - Oy;
    locZ = Z - Oz;
    // To find the contact point in the local (body) frame, just apply inverse quaternion to OP vector in global frame
    applyOriQToVector3<float, deme::oriQ_t>(locX, locY, locZ, oriQw, -oriQx, -oriQy, -oriQz);
    return make_float3(locX, locY, locZ);
}

/// Calculate the contact params based on the 2 contact material types given
template <typename T1>
inline void matProxy2ContactParam(T1& E_eff,
                                  T1& G_eff,
                                  T1& CoR,
                                  T1& mu,
                                  T1& Crr,
                                  const T1& Y1,
                                  const T1& nu1,
                                  const T1& CoR1,
                                  const T1& mu1,
                                  const T1& Crr1,
                                  const T1& Y2,
                                  const T1& nu2,
                                  const T1& CoR2,
                                  const T1& mu2,
                                  const T1& Crr2) {
    T1 invE = (1. - nu1 * nu1) / Y1 + (1. - nu2 * nu2) / Y2;
    E_eff = 1. / invE;
    T1 invG = 2. * (2. - nu1) * (1. + nu1) / Y1 + 2. * (2. - nu2) * (1. + nu2) / Y2;
    G_eff = 1. / invG;
    CoR = min(CoR1, CoR2);
    mu = max(mu1, mu2);
    Crr = max(Crr1, Crr2);
}

/// Calculate the contact params based on the 2 contact material types given (historyless version)
template <typename T1>
inline void matProxy2ContactParam(T1& E_eff,
                                  T1& CoR,
                                  const T1& Y1,
                                  const T1& nu1,
                                  const T1& CoR1,
                                  const T1& Y2,
                                  const T1& nu2,
                                  const T1& CoR2) {
    T1 invE = (1. - nu1 * nu1) / Y1 + (1. - nu2 * nu2) / Y2;
    E_eff = 1. / invE;
    CoR = min(CoR1, CoR2);
}

template <typename T1>
inline __device__ deme::contact_t checkSphereEntityOverlap(const T1& xA,
                                                           const T1& yA,
                                                           const T1& zA,
                                                           const T1& radA,
                                                           const deme::objType_t& typeB,
                                                           const T1& xB,
                                                           const T1& yB,
                                                           const T1& zB,
                                                           const T1& dirxB,
                                                           const T1& diryB,
                                                           const T1& dirzB,
                                                           const T1& size1B,
                                                           const T1& size2B,
                                                           const T1& size3B,
                                                           const bool& normalB,
                                                           const float& beta4Entity) {
    switch (typeB) {
        case (deme::ANAL_OBJ_TYPE_PLANE): {
            const T1 plane2sphX = xA - xB;
            const T1 plane2sphY = yA - yB;
            const T1 plane2sphZ = zA - zB;
            // Plane is directional, and the direction is given by plane rotation
            const T1 dist = dot3<T1>(plane2sphX, plane2sphY, plane2sphZ, dirxB, diryB, dirzB);
            if (dist > radA + beta4Entity) {
                return deme::NOT_A_CONTACT;
            }
            return deme::SPHERE_PLANE_CONTACT;
        }
        case (deme::ANAL_OBJ_TYPE_PLATE): {
            return deme::NOT_A_CONTACT;
        }
        default:
            return deme::NOT_A_CONTACT;
    }
}

// Another version of checkSphereEntityOverlap which gives contact point and contact normal
template <typename T1, typename T2>
inline __device__ deme::contact_t checkSphereEntityOverlap(const T1& xA,
                                                           const T1& yA,
                                                           const T1& zA,
                                                           const T1& radA,
                                                           const deme::objType_t& typeB,
                                                           const T1& xB,
                                                           const T1& yB,
                                                           const T1& zB,
                                                           const T1& dirxB,
                                                           const T1& diryB,
                                                           const T1& dirzB,
                                                           const T1& size1B,
                                                           const T1& size2B,
                                                           const T1& size3B,
                                                           const bool& normalB,
                                                           const float& beta4Entity,
                                                           T1& CPX,
                                                           T1& CPY,
                                                           T1& CPZ,
                                                           T2& normalX,
                                                           T2& normalY,
                                                           T2& normalZ,
                                                           T1& overlapDepth) {
    switch (typeB) {
        case (deme::ANAL_OBJ_TYPE_PLANE): {
            const T1 plane2sphX = xA - xB;
            const T1 plane2sphY = yA - yB;
            const T1 plane2sphZ = zA - zB;
            // Plane is directional, and the direction is given by plane rotation
            const T1 dist = dot3<T1>(plane2sphX, plane2sphY, plane2sphZ, dirxB, diryB, dirzB);
            overlapDepth = (radA + beta4Entity - dist) / 2.0;
            if (overlapDepth < 0.0) {
                return deme::NOT_A_CONTACT;
            }
            // From sphere center, go along negative plane normal for (dist + overlapDepth)
            CPX = xA - dirxB * (dist + overlapDepth);
            CPY = yA - diryB * (dist + overlapDepth);
            CPZ = zA - dirzB * (dist + overlapDepth);
            // Contact normal (B to A) is the same as plane normal
            normalX = dirxB;
            normalY = diryB;
            normalZ = dirzB;
            return deme::SPHERE_PLANE_CONTACT;
        }
        case (deme::ANAL_OBJ_TYPE_PLATE): {
            return deme::NOT_A_CONTACT;
        }
        default:
            return deme::NOT_A_CONTACT;
    }
}
