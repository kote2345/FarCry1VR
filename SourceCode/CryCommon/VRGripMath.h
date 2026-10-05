#pragma once

// Closest horizontal velocity satisfying both held-arm reach planes. Zero
// horizontal speed is always feasible; never create an inward movement command.
inline void ProjectVRGripVelocity(vectorf& velocity, const vectorf normals[2],
	const float limits[2], const bool enabled[2])
{
	const vectorf original = velocity;
	bool outside = false;
	for (int i=0;i<2;++i) if (enabled[i] && velocity*normals[i]>limits[i]) outside = true;
	if (!outside) return;
	vectorf best(0,0,velocity.z);
	float bestDistance = (best-original).len2();
	for (int i=0;i<2;++i) if (enabled[i]) {
		const vectorf candidate = original-normals[i]*max(0.0f,original*normals[i]-limits[i]);
		bool valid = true;
		for (int j=0;j<2;++j) if (enabled[j] && candidate*normals[j]>limits[j]+1.e-5f) valid = false;
		const float distance = (candidate-original).len2();
		if (valid && distance<bestDistance) { best = candidate; bestDistance = distance; }
	}
	if (enabled[0] && enabled[1]) {
		const float dot = normals[0]*normals[1], determinant = 1-dot*dot;
		if (determinant>1.e-6f) {
			const float a = (limits[0]-dot*limits[1])/determinant;
			const float b = (limits[1]-dot*limits[0])/determinant;
			vectorf candidate = normals[0]*a+normals[1]*b;
			candidate.z = original.z;
			const float distance = (candidate-original).len2();
			if (distance<bestDistance) best = candidate;
		}
	}
	velocity = best;
}
