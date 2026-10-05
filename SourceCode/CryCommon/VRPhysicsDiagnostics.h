#pragma once
#include <stdio.h>
#include <stdarg.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

// Call sites bound sample counts. Independent of the engine log verbosity.
inline void VRPhysicsTrace(const char* format, ...)
{
#ifdef __ANDROID__
	va_list logArguments; va_start(logArguments, format);
	__android_log_vprint(ANDROID_LOG_INFO,"VRPhysics",format,logArguments);
	va_end(logArguments);
#endif
#ifdef __ANDROID__
	const char* path = "/sdcard/FarCry/vr_physics_diagnostics.txt";
#else
	const char* path = "vr_physics_diagnostics.txt";
#endif
	FILE* output = fopen(path, "a");
	if (!output) return;
	fputs("[VRPhysics-v6] ", output);
	va_list arguments; va_start(arguments, format);
	vfprintf(output, format, arguments);
	va_end(arguments);
	fputc('\n', output);
	fclose(output);
}
