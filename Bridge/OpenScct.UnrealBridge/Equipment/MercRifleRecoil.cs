using System;
using System.Numerics;
using OpenScct.UnrealBridge.Pose;

namespace OpenScct.UnrealBridge.Equipment;

/// <summary>Local M4 model motion sampled on the same clock as the animated hands. Each shot starts
/// at the pose reached by the previous one. This is separate from the simulation's aim recoil.</summary>
internal struct MercRifleRecoil
{
    private const float HipSwingGain = .75f;
    // The whole hip pose (rearward push, lift, climb, swing and bank) beside the shouldered one: at full scale the
    // rifle jumped around the picture in sustained hip fire. The aimed pose is kept as it is.
    private const float HipMotionScale = .45f;

    // Timing and extrema of the M4 position and rotation animation tracks.
    // Position is arranged in bridge model space (forward, up, right). Angles are pitch, yaw, roll.
    private sealed class Curve(params (float Time, float Amount)[] keys)
    {
        public float Sample(float age, float previous, float amplitude)
        {
            float startTime = 0f;
            float start = previous;
            foreach (var (endTime, amount) in keys)
            {
                float end = amplitude * amount;
                if (age < endTime)
                {
                    float t = Math.Clamp((age - startTime) / (endTime - startTime), 0f, 1f);
                    return float.Lerp(start, end, t * t * (3f - 2f * t));
                }
                startTime = endTime;
                start = end;
            }
            return 0f;
        }
    }

    // PRAS samples the angular track as X roll, Y pitch, Z yaw.
    private static readonly Curve Roll = new((.052f, 1), (.147f, -.464f), (.25f, -.12f), (.35f, .24f), (.50f, .11f), (.72f, -.03f), (1.167f, 0));
    private static readonly Curve Pitch = new((.045f, 1), (.150f, -.439f), (.25f, .13f), (.35f, .13f), (.50f, -.08f), (.72f, .02f), (1.167f, 0));
    private static readonly Curve Yaw = new((.036f, 1), (.165f, -.601f), (.25f, -.03f), (.35f, .23f), (.50f, -.12f), (.72f, .03f), (1.167f, 0));
    private static readonly Curve Rearward = new((.018f, 1), (.10f, .01f), (.157f, -.206f), (.25f, -.11f), (.35f, .06f), (.50f, .05f), (1.167f, 0));
    private static readonly Curve Lift = new((.056f, 1), (.10f, .16f), (.167f, -.54f), (.20f, -.651f), (.25f, -.61f), (.35f, -.36f), (.50f, .09f), (1.167f, 0));

    // Full auto uses the slower single-shot tracks for an opening shot after the timeline has settled.
    private static readonly Curve FirstRoll = new((.086f, 1), (.25f, -.39f), (.35f, .09f), (.50f, .13f), (.75f, -.03f), (1.167f, 0));
    private static readonly Curve FirstHipPitch = new((.060f, .534f), (.17f, -.16f), (.25f, .44f), (.35f, .18f), (.50f, -.13f), (.75f, .03f), (1.167f, 0));
    private static readonly Curve FirstAimPitch = new((.071f, 1), (.17f, -.48f), (.25f, -.14f), (.35f, .28f), (.50f, -.05f), (.75f, .02f), (1.167f, 0));
    private static readonly Curve FirstYaw = new((.108f, 1), (.25f, -.17f), (.35f, -.37f), (.50f, -.06f), (.75f, .03f), (1.167f, 0));
    private static readonly Curve FirstRearward = new((.028f, 1), (.10f, .38f), (.167f, .25f), (.25f, .12f), (.35f, .01f), (.50f, -.08f), (.80f, -.125f), (1.7f, 0));
    private static readonly Curve FirstLift = new((.072f, 1), (.10f, .94f), (.167f, .50f), (.25f, -.17f), (.301f, -.346f), (.35f, -.29f), (.50f, .11f), (1.167f, 0));

    private readonly record struct Pose(float RearCm, float UpCm, float Pitch, float Yaw, float Roll)
    {
        public static Pose Blend(Pose a, Pose b, float t) => new(
            float.Lerp(a.RearCm, b.RearCm, t), float.Lerp(a.UpCm, b.UpCm, t),
            float.Lerp(a.Pitch, b.Pitch, t), float.Lerp(a.Yaw, b.Yaw, t), float.Lerp(a.Roll, b.Roll, t));
    }
    private bool _fired;
    private bool _opening;
    private float _aim;
    private double _start;
    private Pose _from, _amplitude;
    private readonly float Duration => _opening ? 1.7f : 1.167f;

    public void Fire(double time, int shotId, float aimBlend)
    {
        _from = Sample(time);
        _opening = !_fired || time - _start >= Duration;
        _start = time;
        _fired = true;
        _aim = Math.Clamp(aimBlend, 0f, 1f);

        // Deterministic presentation variation. Never take a draw from the weapon's gameplay RNG.
        uint noise = unchecked((uint)shotId * 747796405u + 2891336453u);
        float rear = Random(ref noise), lift = Random(ref noise), pitch = Random(ref noise);
        float yaw = Random(ref noise), roll = Random(ref noise);
        bool yawLeft = Random(ref noise) < .5f, rollLeft = Random(ref noise) < .5f;
        var hip = new Pose(float.Lerp(3, 5, rear), float.Lerp(2, 5, lift), 4f,
            yawLeft ? -float.Lerp(2, 4, yaw) : float.Lerp(2, 3, yaw), rollLeft ? -4f : float.Lerp(2, 3, roll));
        // The reference folds the equipped grip's vertical/horizontal attributes into the aimed
        // pitch and yaw endpoints. Its hip and roll branches read their original values directly.
        var aimed = new Pose(float.Lerp(3, 4, rear), -float.Lerp(.1f, .3f, lift), float.Lerp(.5f, 1.5f, pitch) * .85f,
            (yawLeft ? float.Lerp(-.5f, -.4f, yaw) : float.Lerp(.4f, .5f, yaw)) * 1.20f,
            rollLeft ? float.Lerp(-1.5f, .3f, roll) : float.Lerp(.4f, 1.6f, roll));
        Pose motion = Pose.Blend(Scaled(hip, HipMotionScale), aimed, _aim);
        // Keep the rearward impulse at the hip, but reduce the visible climb, lateral swing and banking.
        // Restore the sighted profile progressively as the shoulder animation seats the stock.
        float swing = float.Lerp(HipSwingGain, 1f, _aim);
        // The normal optic uses the same 1.05 attachment factors at hip and shoulder. During
        // continued fire the authored MultiLoc and MultiRot values also scale rearward, pitch and roll.
        _amplitude = new(motion.RearCm * 1.05f * (_opening ? 1f : 1.2f), motion.UpCm * 1.05f * swing,
            motion.Pitch * (_opening ? 1f : 2.2f) * swing, motion.Yaw * swing, motion.Roll * (_opening ? 1f : 1.5f) * swing);
    }

    private static Pose Scaled(Pose pose, float scale) =>
        new(pose.RearCm * scale, pose.UpCm * scale, pose.Pitch * scale, pose.Yaw * scale, pose.Roll * scale);

    private static float Random(ref uint state)
    {
        state = unchecked(state * 1664525u + 1013904223u);
        return (state >> 8) / 16777216f;
    }

    private readonly Pose Sample(double time)
    {
        float age = (float)(time - _start);
        if (!_fired || age >= Duration) return default;
        if (_opening)
        {
            float pitch = float.Lerp(FirstHipPitch.Sample(age, _from.Pitch, _amplitude.Pitch),
                FirstAimPitch.Sample(age, _from.Pitch, _amplitude.Pitch), _aim);
            return new(FirstRearward.Sample(age, _from.RearCm, _amplitude.RearCm),
                FirstLift.Sample(age, _from.UpCm, _amplitude.UpCm), pitch,
                FirstYaw.Sample(age, _from.Yaw, _amplitude.Yaw), FirstRoll.Sample(age, _from.Roll, _amplitude.Roll));
        }
        return new(Rearward.Sample(age, _from.RearCm, _amplitude.RearCm), Lift.Sample(age, _from.UpCm, _amplitude.UpCm),
            Pitch.Sample(age, _from.Pitch, _amplitude.Pitch), Yaw.Sample(age, _from.Yaw, _amplitude.Yaw),
            Roll.Sample(age, _from.Roll, _amplitude.Roll));
    }

    public readonly Xform TransformAt(double time)
    {
        var p = Sample(time);
        if (p == default) return Xform.Identity;
        const float radians = MathF.PI / 180f;
        Basis3 rotation = SpyFirstPersonRig.Turn(Vector3.UnitY, -p.Yaw * radians) *
            SpyFirstPersonRig.Turn(Vector3.UnitZ, p.Pitch * radians) *
            SpyFirstPersonRig.Turn(Vector3.UnitX, p.Roll * radians);
        return new Xform(rotation, new Vector3(-p.RearCm, p.UpCm, 0f) * .01f);
    }
}
