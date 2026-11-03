# Keep the fetched checkout pristine. CloudSeedCore.cmake applies these small
# portability and initialization fixes only to the build-tree source copy.
if(NOT DEFINED CLOUDSEED_SOURCE_DIR)
    message(FATAL_ERROR "CLOUDSEED_SOURCE_DIR must name the CloudSeedCore build-tree copy")
endif()

function(_cloudseed_replace relative_path before after)
    set(path "${CLOUDSEED_SOURCE_DIR}/${relative_path}")
    file(READ "${path}" source)
    string(FIND "${source}" "${after}" after_position)
    if(NOT after_position EQUAL -1)
        return()
    endif()
    string(FIND "${source}" "${before}" before_position)
    if(before_position EQUAL -1)
        message(FATAL_ERROR "CloudSeedCore patch no longer matches ${relative_path}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${path}" "${source}")
endfunction()

# The upstream example relies on MSVC's strcpy_s and math names that are not
# available in the std namespace on every supported compiler.
file(GLOB_RECURSE _cloudseed_portable_sources
    "${CLOUDSEED_SOURCE_DIR}/*.h" "${CLOUDSEED_SOURCE_DIR}/*.cpp")
foreach(path IN LISTS _cloudseed_portable_sources)
    file(READ "${path}" source)
    foreach(name sinf sqrtf expf)
        string(REPLACE "std::${name}" "::${name}" source "${source}")
    endforeach()
    string(REGEX REPLACE
        "strcpy_s\\(buffer, MAX_STR_SIZE, (\"[^\"]*\")\\)"
        "snprintf(buffer, MAX_STR_SIZE, \"%s\", \\1)" source "${source}")
    string(REPLACE "pow10f(input / 20.0)" "powf(10.0f, input / 20.0)"
        source "${source}")
    string(REPLACE "#define _USE_MATH_DEFINES\n#include"
        "#ifndef _USE_MATH_DEFINES\n#define _USE_MATH_DEFINES\n#endif\n#include"
        source "${source}")
    file(WRITE "${path}" "${source}")
endforeach()

_cloudseed_replace("DSP/Utils.h" [=[#define _USE_MATH_DEFINES 1
#include <math.h>
#include <stdint.h>]=] [=[#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif]=])

# Constructors call coefficient/seed updates before the first preset is loaded.
# Give every value read by those updates a defined starting value.
_cloudseed_replace("DSP/AllpassDiffuser.h" [=[		int delay;
		float modRate;]=] [=[		int delay = 100;
		float modRate = 0.0f;]=])
_cloudseed_replace("DSP/ReverbChannel.h" [=[		int delayLineSeed;
		int postDiffusionSeed;]=] [=[		int delayLineSeed = 0;
		int postDiffusionSeed = 0;]=])
_cloudseed_replace("DSP/ReverbChannel.h" [=[		bool lowCutEnabled;
		bool highCutEnabled;
		bool multitapEnabled;
		bool diffuserEnabled;]=] [=[		bool lowCutEnabled = false;
		bool highCutEnabled = false;
		bool multitapEnabled = false;
		bool diffuserEnabled = false;]=])
_cloudseed_replace("DSP/ReverbChannel.h" [=[			lowPass.SetCutoffHz(20000);
			SetSamplerate(samplerate);]=] [=[			lowPass.SetCutoffHz(20000);
			paramsScaled[Parameter::LateLineDecay] = 0.05;
			SetSamplerate(samplerate);]=])
_cloudseed_replace("DSP/DelayLine.h" [=[		bool DiffuserEnabled;
		bool LowShelfEnabled;
		bool HighShelfEnabled;
		bool CutoffEnabled;
		bool TapPostDiffuser;]=] [=[		bool DiffuserEnabled = false;
		bool LowShelfEnabled = false;
		bool HighShelfEnabled = false;
		bool CutoffEnabled = false;
		bool TapPostDiffuser = false;]=])
_cloudseed_replace("DSP/Biquad.h" [=[		float fs;
		float fsInv;
		float gainDB;
		float q;]=] [=[		float fs = 48000.0f;
		float fsInv = 1.0f / 48000.0f;
		float gainDB = 0.0f;
		float q = 0.5f;]=])
_cloudseed_replace("DSP/Biquad.h" [=[		FilterType Type;
		float Output;
		float Frequency;]=] [=[		FilterType Type = FilterType::LowPass;
		float Output = 0.0f;
		float Frequency = 1000.0f;]=])

# The one-pole filters already protect the Nyquist limit; shelves also need it
# when Pd runs below 40 kHz while normalized frequencies can reach 20 kHz.
_cloudseed_replace("DSP/Biquad.cpp" [=[		auto Fc = Frequency;]=] [=[		auto Fc = Frequency;
		if (Fc >= fs * 0.5f)
			Fc = fs * 0.499f;]=])

# At 192 kHz, 100 ms diffusion plus 2.5 ms modulation (with seeded scaling up
# to 1.15) exceeds the original 100 ms buffer. Reserve 110 ms and leave room
# for the second interpolation sample.
_cloudseed_replace("DSP/ModulatedAllpass.h"
    "static const int DelayBufferSize = 19200; // 100ms at 192Khz"
    "static const int DelayBufferSize = 21120; // 110 ms at 192 kHz, including modulation")
_cloudseed_replace("DSP/ModulatedAllpass.h" [=[				ModAmount = SampleDelay - 1;]=]
    [=[				ModAmount = SampleDelay > 1 ? SampleDelay - 1 : 0;]=])
_cloudseed_replace("DSP/ModulatedAllpass.h" [=[			if (totalDelay <= 0) // should no longer be required
				totalDelay = 1;]=] [=[			if (totalDelay <= 0)
				totalDelay = 1;
			if (totalDelay > DelayBufferSize - 2)
				totalDelay = DelayBufferSize - 2;]=])

unset(_cloudseed_portable_sources)
