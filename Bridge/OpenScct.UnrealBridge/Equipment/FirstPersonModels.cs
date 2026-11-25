using System;
using System.Collections.Generic;
using System.Numerics;
using System.Linq;
using OpenScct.Animation;
using OpenScct.Core;
using OpenScct.Core.Engine;
using OpenScct.Game.Camera;
using OpenScct.Game.Equipment;
using OpenScct.Game.SBase;
using OpenScct.Game.Svm;
using OpenScct.Physics.Movement;
using OpenScct.Server;
using OpenScct.UnrealBridge.Pose;

namespace OpenScct.UnrealBridge.Equipment;

/// <summary>
/// The merc's first-person arms and gun (the weapon's own mesh, SWeaponFirst.1DEF_01 by default): drawn while the
/// local view is first person and the weapon draws its view model (bDrawWeapon), placed at the camera, and posed
/// from the weapon's view model channels (<see cref="ViewModelAnim"/>) by the <see cref="ChannelPoseApplier"/>. The
/// one-shots are sampled after every client tick and drawn between ticks like the camera. Reads the simulation and
/// never writes it.
/// <para>A port of the placing and posing of OpenSCCT's game/Scripts/Views/FirstPersonView.cs. Not ported (they
/// belong to Unreal's drawing): lighting at the eye, the stencil mask, cosmetics and map outfits on the sleeves.</para>
/// <para>A merc watched in a demo's first-person camera gets the same view model, built from his recorded
/// third-person weapon by OpenSCCT's ObservedWeaponViewModel (linked from game/Scripts/Views) and sampled after
/// every tick as FirstPersonView.OnObservedTick and DrawObserved do: <see cref="OnObservedTick"/> and
/// <see cref="DrawObserved"/>.</para>
/// </summary>
internal sealed class MercFirstPerson
{
    private static readonly Name s_firstWaitStFd2 = ViewModelClips.FirstWaitStFd2;

    private readonly ViewModelPose _pose = new();
    private readonly OpenScctGame.Views.ObservedWeaponViewModel _observed = new();
    private readonly ViewModelClipRates _rates = new();
    private readonly Dictionary<string, Loaded?> _loaded = new(StringComparer.OrdinalIgnoreCase);
    private readonly string? _sharedRoot;
    private SWeaponDefense? _lastWeapon;
    private RecoilRecoveryRifle? _trackedRifle;
    private int _trackedRifleShots;
    private MercRifleRecoil _rifleRecoil;
    private int _lastShot;
    private float _revolverKick;

    public MercFirstPerson(string? sharedRoot) => _sharedRoot = sharedRoot;

    /// <summary>The model drawn by the last <see cref="Draw"/>, or null.</summary>
    public EquipmentModel? Model { get; private set; }

    /// <summary>The pose of the last draw (bone by bone relative to the parent), when <see cref="Model"/> is not null.</summary>
    public ChannelPoseApplier? Applier { get; private set; }

    /// <summary>The channels of the last draw (for the probe).</summary>
    public ViewModelPose Pose => _pose;

    /// <summary>
    /// The muzzle ("FX") of the last draw in the camera's frame (metres, X forward, Y up, Z right), and the same bone
    /// in the middle of the idle clip (FirstWaitStFd2), the neutral pose the laser sight is zeroed in; null when the
    /// last draw showed no model or the model has no muzzle bone. The sight's beam is laid from them every frame
    /// (EquipmentFeature.MountedBeam; MercLaserView.LocalBeam of the Godot client).
    /// </summary>
    public (Xform Barrel, Xform? Neutral)? Sight { get; private set; }

    /// <summary>After each client tick: samples the view model channels of the local merc's weapon.</summary>
    public void OnTick(SPlayerController? pc, double time, bool localWeaponView, float aimBlend)
    {
        var weapon = (pc?.Pawn as SBasePawn)?.MyWeapon as SWeaponDefense;
        _pose.OnTick(weapon?.ViewAnim);
        var rifle = weapon as RecoilRecoveryRifle;
        if (!ReferenceEquals(_trackedRifle, rifle))
        {
            _trackedRifle = rifle;
            _trackedRifleShots = rifle?.ShotCounter ?? 0;
            _rifleRecoil = default;
        }
        else if (rifle is not null)
        {
            // The owner's predicted shot is emitted once on the simulation tick. The server's reliable reply
            // confirms that same ID. Sample every tick, even when several ticks run between rendered frames.
            if (!localWeaponView || !rifle.bDrawWeapon || rifle.StateNode is not (SWeaponDefense.s_Selected or SWeaponDefense.s_Firing) ||
                rifle.ShotCounter < _trackedRifleShots)
                _rifleRecoil = default;
            else if (rifle.ShotCounter > _trackedRifleShots)
                _rifleRecoil.Fire(time, rifle.ShotCounter, aimBlend);
            _trackedRifleShots = rifle.ShotCounter;
        }
    }

    /// <summary>
    /// After each client tick of a demo: moves the weapon rebuilt for the watched <paramref name="pawn"/> on by a
    /// tick (its shots and the body's reloads) and samples its channels (FirstPersonView.OnObservedTick).
    /// </summary>
    public void OnObservedTick(SBasePawn? pawn)
    {
        _trackedRifle = null;
        _rifleRecoil = default;
        _pose.OnTick(_observed.Update(pawn, Sim.DeltaTime)?.ViewAnim);
    }

    /// <summary>
    /// The view model of a merc watched from his eyes in a demo, from the weapon rebuilt out of his recorded
    /// third-person one (FirstPersonView.DrawObserved). False when he is not a living merc with a gun in his hands.
    /// </summary>
    public bool DrawObserved(SBasePawn? pawn, float dt, float alpha, ref BridgeFirstPerson view)
    {
        Model = null;
        Applier = null;
        SWeaponDefense? weapon = _observed.Update(pawn, 0f);
        if (!ReferenceEquals(_lastWeapon, weapon))
        {
            _pose.OnTick(weapon?.ViewAnim);
        }

        return weapon is not null && !weapon.Mesh.IsNone && DrawWeapon(weapon, _observed.ShotCount, dt, alpha, ref view);
    }

    /// <summary>
    /// Poses the view model of <paramref name="pc"/>'s weapon for a frame <paramref name="dt"/> seconds after the
    /// last and <paramref name="alpha"/> of the way into the next tick, and fills <paramref name="view"/>. False when
    /// no view model is drawn (third person, no weapon, weapon put away, the file missing).
    /// (Estimated in OpenSCCT: the view model sits at the camera's location and rotation; the original's weapon draw
    /// offset is not in the specs.)
    /// </summary>
    public bool Draw(SPlayerController? pc, float dt, float alpha, ref BridgeFirstPerson view, float aimBlend,
        bool localWeaponView, double time)
    {
        Model = null;
        Applier = null;
        Sight = null;
        var weapon = (pc?.Pawn as SBasePawn)?.MyWeapon as SWeaponDefense;
        bool show = pc is { bBehindView: false } && weapon is { bDrawWeapon: true, bDeleteMe: false } && !weapon.Mesh.IsNone &&
            ReferenceEquals(pc.ViewTarget ?? pc.Pawn, pc.Pawn);
        return show && DrawWeapon(weapon!, weapon!.ShotCounter, dt, alpha, ref view, aimBlend,
            localWeaponView && _trackedRifle is not null && ReferenceEquals(weapon, _trackedRifle), time);
    }

    // How strongly the shot clip moves a rifle without MercRifleRecoil (AK-47, compact SMG) from the hip. The precision
    // rifles keep theirs: it works their bolt.
    private const float HipShotClipScale = .5f;
    private ChannelPose[] _hipShot = [];

    private bool DrawWeapon(SWeaponDefense weapon, int shotCount, float dt, float alpha, ref BridgeFirstPerson view,
        float aimBlend = 0f, bool useRifleRecoil = false, double time = 0)
    {
        if (Load(weapon.Mesh.Path!) is not { } loaded)
        {
            return false;
        }

        if (!ReferenceEquals(_lastWeapon, weapon))
        {
            _lastWeapon = weapon;
            _lastShot = shotCount;
            _revolverKick = 0f;
        }

        if (weapon is MercSidearm { Kind: (int)SidearmKind.Magnum44 } && shotCount != _lastShot)
        {
            _revolverKick = 0.42f;
        }

        _lastShot = shotCount;

        // A pistol's reload, and a reload of OpenSCCT's own rifles, owns both arms: nothing else is layered over it.
        Name action = weapon.ViewAnim.Sequence(SWeaponDefense.ACTIONCHANNEL);
        bool customHandling = weapon is MercSidearm or MercRifle or PumpShotgun &&
            (weapon is MercSidearm { ReloadUntil: > 0 } || action == weapon.AFirstReloGrenSt || action == weapon.AFirstReloSt);
        // A clip of an animation project bound to this model for gameplay runs at the project clip's frame rate.
        _rates.Bind(weapon.XLevel?.GetService<AnimationWorld>()?.Project, weapon.Mesh.Path!);
        loaded.Clips.Bind(_rates.Project, weapon.Mesh.Path!);
        var channels = _pose.Update(weapon.ViewAnim, loaded.Model.Character, dt, alpha, exclusiveAction: customHandling,
            blendLandingRecovery: weapon is MercSidearm, blendLocomotion: true, rates: _rates, replacements: loaded.Clips);
        Xform removeClipKick = Xform.Identity;
        if (useRifleRecoil && action == weapon.AFirstShooSt && loaded.Model.MuzzleBone >= 0)
        {
            var skeleton = loaded.Model.ActorToSkeleton;
            removeClipKick = skeleton * loaded.Applier.ApplyWithAnchor(channels, SWeaponDefense.ACTIONCHANNEL,
                loaded.Model.MuzzleBone) * skeleton.AffineInverse();
        }
        else if (weapon is MercRifle and not (RecoilRecoveryRifle or PrecisionRifle) && action == weapon.AFirstShooSt && aimBlend < 1f)
        {
            // The other rifles' shot clip moves the whole gun. From the hip it is drawn weaker (MercRifleRecoil's
            // HipMotionScale is the M4 family's), and at full strength once shouldered, where the sights follow it.
            _hipShot = channels.ToArray();
            _hipShot[SWeaponDefense.ACTIONCHANNEL].Alpha *= float.Lerp(HipShotClipScale, 1f, Math.Clamp(aimBlend, 0f, 1f));
            loaded.Applier.Apply(_hipShot);
        }
        else
        {
            loaded.Applier.Apply(channels);
        }

        Xform root = Xform.Identity;
        if (weapon is MercSidearm && loaded.SightOffset is { } sight)
        {
            root *= sight;
        }
        if ((weapon.AttachmentMask & WeaponAttachments.Acog) != 0 && loaded.AcogAim is { } scope)
        {
            root = EquipmentMath.Interpolate(root, scope, aimBlend);
        }
        else if (((weapon is M4A1S && (weapon.AttachmentMask & WeaponAttachments.Optics) == 0) ||
                  (weapon is PrecisionRifle or MercSidearm && (weapon.AttachmentMask & WeaponAttachments.RedDot) != 0)) &&
                 loaded.CloseSightAim is { } closeSight)
        {
            // These sights share a 12 cm eye position. The M4's rear opening is
            // framed to occupy about 2.8% of the mercenary view's width.
            root = EquipmentMath.Interpolate(root, closeSight, aimBlend);
        }
        else if (weapon is MercRifle or PumpShotgun && loaded.SightOffset is { } ads)
        {
            root *= EquipmentMath.Interpolate(Xform.Identity, ads, aimBlend);
        }
        else if ((weapon is MercSidearm || (weapon is not MercRifle && (weapon.AttachmentMask & WeaponAttachments.RedDot) != 0)) &&
                 loaded.BoreAim is { } boreAim)
        {
            // These rigs only have FX. The native presenter aligns either the physical
            // iron sight line or the mounted glass to this animated reference.
            root = EquipmentMath.Interpolate(root, boreAim, aimBlend);
        }

        if (_revolverKick > 0f)
        {
            // The actor's X is forward and its Z is screen-right, so the muzzle climbs about Z; a turn about X would
            // only roll the gun.
            root = new Xform(root.Basis * SpyFirstPersonRig.Turn(Vector3.UnitZ, _revolverKick) *
                SpyFirstPersonRig.Turn(Vector3.UnitY, -_revolverKick * 0.45f), root.Origin);
            _revolverKick *= MathF.Exp(-10f * dt);
        }

        Xform neutralRoot = root;
        if (useRifleRecoil)
        {
            root = _rifleRecoil.TransformAt(time) * root * removeClipKick;
        }
        Model = loaded.Model;
        Applier = loaded.Applier;
        Sight = loaded.Model.MuzzleBone >= 0
            ? (root * loaded.Model.ActorToSkeleton * loaded.Model.Global(loaded.Model.MuzzleBone, loaded.Applier.Positions,
                loaded.Applier.Rotations, loaded.Applier.Scales), loaded.NeutralMuzzle is { } neutral ? neutralRoot * neutral : null)
            : null;
        view.Kind = BridgeFirstPersonKind.Merc;
        view.PartCount = 1;
        view.WeaponClass = EquipmentNames.ClassId(weapon);
        // The rifle's rate-of-fire screen is Skins[3], which SetMode swaps between SI_rof_single, SI_rof_burst and
        // SI_rof_auto (spec/equipment/merc-rifle.md rule 16).
        view.RateOfFireSkin = weapon.Skins.Count > SWeaponDefense.ROF_MaterialID
            ? EquipmentNames.Id(weapon.Skins[SWeaponDefense.ROF_MaterialID].Path) : 0;
        view.Part0 = FirstPersonParts.Part(loaded.Model, root, loaded.Applier.Positions, loaded.Applier.Rotations, loaded.Applier.Scales);
        return true;
    }

    private Loaded? Load(string asset)
    {
        if (_loaded.TryGetValue(asset, out Loaded? known))
        {
            return known;
        }

        Loaded? loaded = null;
        if (EquipmentModel.Load(asset, _sharedRoot) is { } model)
        {
            Xform? neutral = IdleSocket(model, model.MuzzleBone, .5f);
            bool pistol = asset.StartsWith("OpenScctSidearms.", StringComparison.OrdinalIgnoreCase);
            Xform? boreAim = neutral is { } bore ? AlignSight(bore, pistol ? .38f : .24f) : null;
            Xform? ads = IdleSocket(model, model.Character.FindBone("ADS"), 0f);
            // The 4x ocular needs consistent eye relief across receivers. The AK's
            // native 11 cm iron sight distance clips the scope against the screen edges.
            Xform? acogAim = (ads ?? neutral) is { } anchor ? AlignSight(anchor, pistol ? .22f : .26f) : null;
            Xform? closeReferenceSocket = pistol ? neutral : ads;
            Xform? closeSightAim = closeReferenceSocket is { } closeReference ? AlignSight(closeReference, .12f) : null;
            loaded = new Loaded(model, new ChannelPoseApplier(model.Character), SidearmSight(asset, neutral) ?? RifleSight(asset, ads), neutral, boreAim, acogAim, closeSightAim, new StudioClips(model.Character));
        }

        _loaded[asset] = loaded;
        return loaded;
    }

    /// <summary>
    /// A pistol's sights set on a point ten metres ahead, worked out once from where its muzzle sits in the idle
    /// clip (OpenSCCT's own pistols; port of FirstPersonView.SidearmSightOffset). Null for the game's own guns.
    /// </summary>
    private static Xform? SidearmSight(string asset, Xform? neutralMuzzle)
    {
        if (!asset.StartsWith("OpenScctSidearms.", StringComparison.OrdinalIgnoreCase) || neutralMuzzle is not { } neutral)
        {
            return null;
        }

        Vector3 direction = Vector3.Normalize(new Vector3(10f, 0f, 0f) - neutral.Origin);
        Basis3 rotation = Basis3.FromQuaternion(EquipmentMath.ShortestArc(neutral.Basis.X, direction));
        return new Xform(rotation, neutral.Origin - (rotation * neutral.Origin));
    }

    /// <summary>
    /// The muzzle ("FX") where it sits in the middle of the idle clip, in the model's actor frame: the neutral pose
    /// the pistols' sights and every gun's laser sight are set in. Null without the bone or the clip.
    /// </summary>
    private static Xform? IdleSocket(EquipmentModel model, int bone, float phase)
    {
        if (bone < 0 || model.Character.Find(s_firstWaitStFd2.ToString()) is not { } idle)
        {
            return null;
        }

        int count = model.Character.BoneCount;
        var positions = new Vector3[count];
        var rotations = new Quaternion[count];
        var scales = new Vector3[count];
        model.Sample(idle, phase, positions, rotations, scales);
        return model.ActorToSkeleton * model.Global(bone, positions, rotations, scales);
    }

    private static Xform? RifleSight(string asset, Xform? ads)
    {
        MercRifleDefinition? rifle = MercRifles.All.FirstOrDefault(r => string.Equals(r.Asset + "_first", asset, StringComparison.OrdinalIgnoreCase));
        if (rifle is null || ads is not { } sight)
        {
            return null;
        }

        return AlignSight(sight, rifle.EyeRelief);
    }

    private static Xform AlignSight(Xform sight, float eyeRelief)
    {
        Basis3 rotation = Basis3.FromQuaternion(EquipmentMath.ShortestArc(sight.Basis.X, Vector3.UnitX));
        return new Xform(rotation, Vector3.UnitX * eyeRelief - rotation * sight.Origin);
    }

    private sealed record Loaded(EquipmentModel Model, ChannelPoseApplier Applier, Xform? SightOffset, Xform? NeutralMuzzle, Xform? BoreAim, Xform? AcogAim, Xform? CloseSightAim, StudioClips Clips);
}

/// <summary>
/// The pistol and its grip in the local spy's eye view, relative to the camera: his arms (SPerso.ATT_01_fp) in the
/// stock two-handed grip fitted once at load (<see cref="SpyFirstPersonRig"/>) and the pistol (SWeaponFirst.p190_fp)
/// in his right hand. While drawing or holstering both swing between a lowered place and the grip; the pistol plays
/// its own draw clip.
/// <para>A port of the placing and posing of OpenSCCT's game/Scripts/Views/SpyFirstPersonView.cs (the eye view is
/// OpenSCCT's own). With the eye of the frame it also traces from the eye to the laser and muzzle sockets
/// (TryGetSocket): a socket with a wall between it and the eye is flagged hidden, and Unreal then starts neither the
/// muzzle flash nor the laser there. Studio profiles can supply both hand and weapon poses; the native renderer
/// dresses and lights the resulting meshes.</para>
/// </summary>
internal sealed class SpyFirstPerson
{
    private const string ArmsAsset = "SPerso.ATT_01_fp";
    private const string WeaponAsset = "SWeaponFirst.p190_fp";
    private static readonly FVector s_probeExtent = new(1.25f, 1.25f, 1.25f);

    // X is forward, Y is up and Z is screen-right. Distances are metres from the eye.
    private static readonly Xform s_lowered = new(
        SpyFirstPersonRig.Turn(Vector3.UnitY, -.23f) * SpyFirstPersonRig.Turn(Vector3.UnitZ, -.44f) * SpyFirstPersonRig.Turn(Vector3.UnitX, .30f),
        new Vector3(.20f, -.35f, .27f));

    private readonly string? _sharedRoot;
    private bool _preloaded;
    private Xform _gripped;
    private Xform _gripInverse;
    private int _stockBone = -1;
    private Vector3 _stockRest;
    private Name _sampledClip;
    private float _sampledPhase = float.NaN;
    private StudioClips? _weaponClips;
    private StudioClips? _armClips;
    private float _studioClock, _fireAge = float.PositiveInfinity, _lastShotTime = float.NegativeInfinity;
    private bool _studioArms, _studioGun;
    private Vector3[] _gripPositions = [], _gripScales = [];
    private Quaternion[] _gripRotations = [];
    private string? _sampledName;

    public SpyFirstPerson(string? sharedRoot) => _sharedRoot = sharedRoot;

    public EquipmentModel? Arms { get; private set; }

    public EquipmentModel? Weapon { get; private set; }

    /// <summary>The arms' pose, fixed once the grip is fitted; the pistol's, sampled each draw.</summary>
    public Vector3[] ArmPositions { get; private set; } = [];

    public Quaternion[] ArmRotations { get; private set; } = [];

    public Vector3[] ArmScales { get; private set; } = [];

    public Vector3[] WeaponPositions { get; private set; } = [];

    public Quaternion[] WeaponRotations { get; private set; } = [];

    public Vector3[] WeaponScales { get; private set; } = [];

    /// <summary>How far the support wrist ended from the stock stance's place when the grip was fitted, metres (-1 before).</summary>
    public float SupportContactError { get; private set; } = -1f;

    /// <summary>Reads the two models and fits the grip (on first use; a content without them leaves the view off).</summary>
    public void Preload()
    {
        if (_preloaded)
        {
            return;
        }

        _preloaded = true;
        if (EquipmentModel.Load(ArmsAsset, _sharedRoot) is not { } arms || EquipmentModel.Load(WeaponAsset, _sharedRoot) is not { } gun ||
            gun.LaserBone < 0 || gun.MuzzleBone < 0)
        {
            return;
        }

        var positions = new Vector3[arms.Character.BoneCount];
        var rotations = new Quaternion[positions.Length];
        var scales = new Vector3[positions.Length];
        if (!SpyFirstPersonRig.TryPose(arms, SpyFirstPersonRig.Ready, positions, rotations, scales, out _gripped, out float error))
        {
            return;
        }

        _gripInverse = _gripped.AffineInverse();
        SupportContactError = error;
        _gripPositions = positions;
        _gripRotations = rotations;
        _gripScales = scales;
        ArmPositions = (Vector3[])positions.Clone();
        ArmRotations = (Quaternion[])rotations.Clone();
        ArmScales = (Vector3[])scales.Clone();
        WeaponPositions = (Vector3[])gun.Character.RestPosition.Clone();
        WeaponRotations = (Quaternion[])gun.Character.RestRotation.Clone();
        WeaponScales = (Vector3[])gun.Character.RestScale.Clone();
        _stockBone = gun.Character.FindBone("crosse");
        if (_stockBone >= 0)
        {
            _stockRest = gun.Character.RestPosition[_stockBone];
        }

        _weaponClips = new StudioClips(gun.Character);
        _armClips = new StudioClips(arms.Character);
        Arms = arms;
        Weapon = gun;
    }

    /// <summary>
    /// Places and poses the eye view of <paramref name="pawn"/> for a frame <paramref name="alpha"/> of the way into
    /// the next tick, and fills <paramref name="view"/>. False when it is not drawn (no pistol out, a move that
    /// hides it, the models missing).
    /// </summary>
    public bool Draw(SPlayerController pc, SPawnAttaque pawn, float alpha, ref BridgeFirstPerson view,
        (FVector Location, FRotator Rotation)? eye = null, float dt = 0f)
    {
        MuzzleHidden = false;
        LaserHidden = false;
        MuzzleDistance = 0f;
        Preload();
        if (Arms is not { } arms || Weapon is not { } gun ||
            pawn is not { bDeleteMe: false, bHidden: false, Health: > 0 } || !ReferenceEquals(pc.Pawn, pawn))
        {
            return false;
        }

        // The stance can arrive before the weapon references on the owning client.
        SWeaponAttaque? pistol = pawn.MyWeapon is SWeaponAttaque { bDeleteMe: false } own ? own
            : pawn.WeaponDummy is { bDeleteMe: false } dummy ? dummy : null;
        if (pawn.WeaponLoaded == 0 || HideForAction(pc, pawn))
        {
            return false;
        }

        var project = pawn.XLevel?.GetService<AnimationWorld>()?.Project;
        if (_weaponClips!.Bind(project, WeaponAsset) || _studioGun) _sampledPhase = float.NaN;
        var armsChanged = _armClips!.Bind(project, ArmsAsset);
        if (armsChanged)
        {
            SpyFirstPersonRig.TryPose(arms, SpyFirstPersonRig.Ready, _gripPositions, _gripRotations, _gripScales,
                out _gripped, out float error, _armClips);
            SupportContactError = error;
            _gripInverse = _gripped.AffineInverse();
        }
        if (armsChanged || _studioArms)
        {
            _gripPositions.CopyTo(ArmPositions, 0);
            _gripRotations.CopyTo(ArmRotations, 0);
            _gripScales.CopyTo(ArmScales, 0);
        }
        _studioArms = _studioGun = false;
        bool drawing = pawn.WeaponLoaded is 3 or 4;
        float amount = drawing ? DrawAmount(pawn, pistol?.ViewAnim, alpha) : 1f;
        float eased = amount * amount * (3f - (2f * amount));
        Xform gunRoot = drawing ? EquipmentMath.Interpolate(s_lowered, _gripped, eased) : _gripped;
        Xform armsRoot = drawing ? gunRoot * _gripInverse : Xform.Identity;
        PoseWeapon(gun, drawing ? ViewModelClips.DrawP190 : ViewModelClips.SnipeP190, drawing ? amount : 0f);

        var profile = project?.FirstPersonProfiles.FindLast(p => p.Enabled && p.Role == FirstPersonRole.Spy && p.MainAsset == WeaponAsset);
        if (project is not null && profile is not null)
        {
            if (project.Scene.Find(rig => rig.Id == profile.MainRig) is { } main)
            {
                var mount = Xform.FromTrs(main.Transform.Position, main.Transform.Rotation, main.Transform.Scale);
                gunRoot = drawing ? EquipmentMath.Interpolate(s_lowered, mount, eased) : mount;
                var support = project.Scene.Find(rig => rig.Id == profile.ArmsRig)?.Transform ?? BoneTransform.Identity;
                armsRoot = Xform.FromTrs(support.Position, support.Rotation, support.Scale);
                if (drawing) armsRoot = gunRoot * mount.AffineInverse() * armsRoot;
            }
            _studioClock += dt;
            _fireAge += dt;
            if (pistol is not null && pistol.LastShot != _lastShotTime)
            {
                _lastShotTime = pistol.LastShot;
                float age = pawn.Level.TimeSeconds - pistol.LastShot;
                if (!drawing && age >= 0f && age < .1f) _fireAge = 0f;
            }
            string slot = drawing ? pawn.WeaponLoaded == 4 ? "HolsterP190" : "DrawP190"
                : _fireAge < FirstPersonEvaluation.Duration(project, profile, "FireP190") ? "FireP190"
                : pawn.WeaponLoaded == 2 ? "AimP190" : pawn.Velocity.SizeSquared() > 100 ? "MoveP190" : "SnipeP190";
            float duration = MathF.Max(FirstPersonEvaluation.Duration(project, profile, slot), .0001f);
            float phase = drawing ? pawn.WeaponLoaded == 4 ? 1f - amount : amount
                : slot == "FireP190" ? _fireAge / duration : (_studioClock % duration) / duration;
            _studioGun = StudioClips.ApplyPose(project, FirstPersonEvaluation.Evaluate(project, profile, slot, phase, false),
                gun.Character, WeaponPositions, WeaponRotations, WeaponScales);
            _studioArms = StudioClips.ApplyPose(project, FirstPersonEvaluation.Evaluate(project, profile, slot, phase, true),
                arms.Character, ArmPositions, ArmRotations, ArmScales);
        }

        view.Kind = BridgeFirstPersonKind.Spy;
        view.PartCount = 2;
        view.WeaponClass = EquipmentNames.ClassId(pistol);
        view.Part0 = FirstPersonParts.Part(arms, armsRoot, ArmPositions, ArmRotations, ArmScales);
        view.Part1 = FirstPersonParts.Part(gun, gunRoot, WeaponPositions, WeaponRotations, WeaponScales);
        if (eye is { } camera)
        {
            MuzzleHidden = Behind(pawn, camera, gun, gunRoot, gun.MuzzleBone, out float muzzleDistance);
            LaserHidden = Behind(pawn, camera, gun, gunRoot, gun.LaserBone, out _);
            MuzzleDistance = muzzleDistance;
        }

        return true;
    }

    /// <summary>True when the last <see cref="Draw"/> found a wall between the eye and the pistol's muzzle socket "FX".</summary>
    public bool MuzzleHidden { get; private set; }

    /// <summary>True when the last <see cref="Draw"/> found a wall between the eye and the pistol's laser socket "Laser".</summary>
    public bool LaserHidden { get; private set; }

    /// <summary>How far the muzzle socket was from the eye in the last <see cref="Draw"/>, in units.</summary>
    public float MuzzleDistance { get; private set; }

    /// <summary>
    /// SpyFirstPersonView.TryGetSocket: whether the client world's collision (the camera's trace flags, a 1.25 unit
    /// box) is between the eye and a socket of the pistol as drawn. Only the effects that start at the socket are
    /// moved off it (the muzzle flash, the laser's start); the pistol itself stays fixed to the eye.
    /// </summary>
    private bool Behind(SPawnAttaque pawn, (FVector Location, FRotator Rotation) eye, EquipmentModel gun, in Xform root,
        int bone, out float distance)
    {
        distance = 0f;
        if (bone < 0 || pawn.XLevel?.PhysicsDriver is not PhysicsDriver physics)
        {
            return false;
        }

        // The camera's frame of the parts: metres, X forward, Y up, Z right.
        Vector3 local = (root * gun.ActorToSkeleton * gun.Global(bone, WeaponPositions, WeaponRotations, WeaponScales)).Origin;
        eye.Rotation.GetAxes(out FVector forward, out FVector right, out FVector up);
        FVector socket = eye.Location + (((forward * local.X) + (up * local.Y) + (right * local.Z)) * RenderSpace.UnitsPerMetre);
        distance = (socket - eye.Location).Size();
        return physics.Collision.LineCheck(out _, null, socket, eye.Location, s_probeExtent, CameraK.CameraTraceFlags);
    }

    /// <summary>The moves during which the eye view shows no pistol: using objects, hacking, bombs, rolls, ladders, pipes, ledges.</summary>
    private static bool HideForAction(SPlayerController pc, SPawnAttaque pawn) =>
        pc.StateNode is SPlayerController.s_InteractWithObject or SPlayerController.s_InteractWithObjectWaitingEvent or
            SPlayerController.s_Computer_Enter or SPlayerController.s_Computer_Use or SPlayerController.s_Computer_Leave or
            SPlayerController.s_Computer_FastLeave or SPlayerController.s_Hack_Enter or SPlayerController.s_Hack_Use or
            SPlayerController.s_Hack_Leave or SPlayerController.s_Hack_FastLeave or SPlayerController.s_Maintain_Enter or
            SPlayerController.s_Maintain_Use or SPlayerController.s_Maintain_Leave or SPlayerController.s_Pickup or
            SPlayerController.s_PutFlagInDropZone or SPlayerController.s_PlaceBomb or SPlayerController.s_PlaceBomb_End or
            SPlayerController.s_PlayerRolling ||
        pawn.Physics is EPhysics.PHYS_OnLadder or EPhysics.PHYS_OnPipe or EPhysics.PHYS_OnPipeH or EPhysics.PHYS_Linear or
            EPhysics.PHYS_OnVerticalPlane or EPhysics.PHYS_Ledge or EPhysics.PHYS_Vandamme;

    /// <summary>
    /// How far the draw has come, 0 lowered to 1 gripped: from the pistol's own draw clip when it plays (backward for
    /// the holster), else from the body's draw clip.
    /// </summary>
    private static float DrawAmount(SPawnAttaque pawn, ViewModelAnim? anim, float alpha)
    {
        const int channel = SWeaponAttaque.DEFAULTCHANNEL;
        if (anim is not null && anim.Sequence(channel) == ViewModelClips.DrawP190)
        {
            float duration = anim.Data(channel).Seconds(anim.Rate(channel));
            if (duration > 0f)
            {
                float remaining = anim.Remaining(channel) + ((1f - Math.Clamp(alpha, 0f, 1f)) * Sim.DeltaTime);
                float progress = Math.Clamp(1f - (remaining / duration), 0f, 1f);
                return anim.IsBackward(channel) ? 1f - progress : progress;
            }
        }

        var body = pawn.PawnAnim[0];
        Name clip = body.Sequence;
        if (!body.bPlaying || body.EndFrame <= 0f ||
            (clip != pawn.ADrawCrouch2H && clip != pawn.ADrawStand2H && clip != pawn.AUnDrawCrouch2H && clip != pawn.AUnDrawStand2H))
        {
            return 1f;
        }

        float phase = Math.Clamp(body.Frame / body.EndFrame, 0f, 1f);
        return pawn.WeaponLoaded == 4 ? 1f - phase : phase;
    }

    private void PoseWeapon(EquipmentModel model, Name clip, float phase)
    {
        if (clip == _sampledClip && phase == _sampledPhase)
        {
            return;
        }

        if (clip != _sampledClip)
        {
            _sampledName = clip.ToString();
        }

        _sampledClip = clip;
        _sampledPhase = phase;
        if (_weaponClips!.Find(_sampledName!) is not { } sequence)
        {
            return;
        }

        model.Sample(sequence, phase, WeaponPositions, WeaponRotations, WeaponScales);
        if (_stockBone >= 0)
        {
            // Shorten the visible stock travel to keep it below the HUD.
            WeaponPositions[_stockBone] = Vector3.Lerp(_stockRest, WeaponPositions[_stockBone], .34f);
        }
    }
}

internal static class FirstPersonParts
{
    /// <summary>One model of the first-person view: where its skeleton is in the camera's frame, and its muzzle and laser socket as posed.</summary>
    public static BridgeFirstPersonPart Part(EquipmentModel model, in Xform root, ReadOnlySpan<Vector3> positions,
        ReadOnlySpan<Quaternion> rotations, ReadOnlySpan<Vector3> scales)
    {
        Xform toCamera = root * model.ActorToSkeleton;
        var part = new BridgeFirstPersonPart
        {
            Mesh = model.AssetId,
            MeshPath = model.PathId,
            BoneCount = model.Character.BoneCount,
            MuzzleBone = model.MuzzleBone,
            LaserBone = model.LaserBone,
            ModelToCamera = EquipmentMath.ToBone(toCamera),
        };
        if (model.MuzzleBone >= 0)
        {
            part.Muzzle = EquipmentMath.ToBone(toCamera * model.Global(model.MuzzleBone, positions, rotations, scales));
        }

        if (model.LaserBone >= 0)
        {
            part.Laser = EquipmentMath.ToBone(toCamera * model.Global(model.LaserBone, positions, rotations, scales));
        }

        return part;
    }
}
