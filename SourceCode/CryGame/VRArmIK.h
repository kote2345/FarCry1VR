#ifndef CRYGAME_VR_ARM_IK_H
#define CRYGAME_VR_ARM_IK_H

inline bool SolveVRArm(const Vec3& shoulder, const Vec3& wristTarget,
	const Vec3& elbowPole, float upperLength, float forearmLength,
	Vec3& elbow, Vec3& reachableWrist)
{
	if (upperLength <= 1.0e-4f || forearmLength <= 1.0e-4f)
		return false;
	Vec3 direction = wristTarget - shoulder;
	float distance = direction.GetLength();
	if (distance > 1.0e-4f)
		direction /= distance;
	else
	{
		direction.Set(0, 0, 1);
		distance = 0.0f;
	}
	const float minReach = fabsf(upperLength - forearmLength) + 1.0e-4f;
	const float maxReach = upperLength + forearmLength - 1.0e-4f;
	const float reach = max(minReach, min(maxReach, distance));
	reachableWrist = shoulder + direction * reach;

	Vec3 pole = elbowPole - shoulder;
	pole -= direction * (pole | direction);
	float poleLength = pole.GetLength();
	if (poleLength < 1.0e-4f)
	{
		pole = direction.Cross(Vec3(0, 1, 0));
		poleLength = pole.GetLength();
		if (poleLength < 1.0e-4f)
		{
			pole = direction.Cross(Vec3(1, 0, 0));
			poleLength = pole.GetLength();
		}
	}
	if (poleLength < 1.0e-4f)
		return false;
	pole /= poleLength;
	const float along = (upperLength * upperLength - forearmLength * forearmLength + reach * reach) /
		(2.0f * reach);
	const float height = cry_sqrtf(max(0.0f, upperLength * upperLength - along * along));
	elbow = shoulder + direction * along + pole * height;
	return true;
}

#endif
