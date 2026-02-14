#pragma once

#include <d3dcompiler.h>

// Experimental depth-based stereo shader variants.
// NOTE: Namespace cannot start with a digit, so we use ThreePassShader.
namespace ThreePassShader {

// Output blob must be released by the caller.
// Compiles only the parallax SBS pass (CSParallaxSbs).
bool CompileDepthRawCS(ID3DBlob** outCsBlob);

bool CompileDepthSmoothCS(ID3DBlob** outCsBlob);

bool CompileParallaxSbsCS(ID3DBlob** outCsBlob);

}
