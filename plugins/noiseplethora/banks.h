// plugins/noiseplethora/banks.h - Noise Plethora's sound-generating algorithms (the
// 21 of banks 1..3 at 6dea196 that use no freeverb/granular/delay/flange effect),
// grouped into packages (docs/plugins.md 10). The eight small ones (make packages) are
// at most ~13 KB of arena each, so two or three share the Teensy's 32 KB arena; A, B
// and C (make legacy) are the first grouping, kept so projects that use them still load.
// X(class, output gain, ALG name): the gain is NP's own (Banks_Def.hpp); the name (<= 7
// characters) is the one in the package's manifest (<pkg>.json; a test checks they agree).
// The order is the ALG order.
// The Makefile reads the class names from these lines: keep one #define per line
// (NP_PKG_<PKG in upper case>; the Makefile also sets each package's block size).
#pragma once
#define NP_PKG_A(X) X(PrimeCluster, 0.8f, "PrimeCl") X(PrimeCnoise, 0.8f, "PrimeCn") X(FibonacciCluster, 1.0f, "Fibonac") X(partialCluster, 1.0f, "Partial")
#define NP_PKG_B(X) X(phasingCluster, 1.0f, "Phasing") X(clusterSaw, 1.0f, "ClstSaw") X(sineFMcluster, 1.0f, "SineFM") X(TriFMcluster, 1.0f, "TriFM") X(crCluster2, 1.0f, "CrClus2") X(pwCluster, 1.0f, "PwClust")
#define NP_PKG_C(X) X(radioOhNo, 1.0f, "RadioOh") X(xModRingSqr, 1.0f, "XModSqr") X(XModRingSine, 1.0f, "XModSin") X(CrossModRing, 1.0f, "CrossMR") X(resonoise, 1.0f, "Resonoi") X(basurilla, 1.0f, "Basuril") X(Atari, 1.0f, "Atari") X(WalkingFilomena, 1.0f, "Walking") X(arrayOnTheRocks, 1.0f, "ArrayOR") X(existencelsPain, 1.0f, "ExistPn") X(whoKnows, 1.0f, "WhoKnow")
// the small packages (2026-10-18): same algorithms, same block sizes as in A, B, C
#define NP_PKG_PRIME(X) X(PrimeCluster, 0.8f, "PrimeCl") X(PrimeCnoise, 0.8f, "PrimeCn")
#define NP_PKG_FIBO(X) X(FibonacciCluster, 1.0f, "Fibonac") X(partialCluster, 1.0f, "Partial")
#define NP_PKG_PHASE(X) X(phasingCluster, 1.0f, "Phasing") X(clusterSaw, 1.0f, "ClstSaw")
#define NP_PKG_FM(X) X(sineFMcluster, 1.0f, "SineFM") X(TriFMcluster, 1.0f, "TriFM")
#define NP_PKG_PW(X) X(crCluster2, 1.0f, "CrClus2") X(pwCluster, 1.0f, "PwClust")
#define NP_PKG_RING(X) X(radioOhNo, 1.0f, "RadioOh") X(xModRingSqr, 1.0f, "XModSqr") X(XModRingSine, 1.0f, "XModSin") X(CrossModRing, 1.0f, "CrossMR") X(arrayOnTheRocks, 1.0f, "ArrayOR")
#define NP_PKG_NOISE(X) X(resonoise, 1.0f, "Resonoi") X(basurilla, 1.0f, "Basuril") X(Atari, 1.0f, "Atari") X(existencelsPain, 1.0f, "ExistPn") X(whoKnows, 1.0f, "WhoKnow")
#define NP_PKG_WALK(X) X(WalkingFilomena, 1.0f, "Walking")
