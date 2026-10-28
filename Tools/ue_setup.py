"""
One-time editor setup for the Generals recreation (run inside Unreal's Python):

  UnrealEditor-Cmd.exe Generals1.uproject -run=pythonscript -script="<path>/ue_setup.py"

Creates under /Game/Generals:
  Materials/M_W3D_Lit, M_W3D_Unlit         base materials for W3D meshes
  Materials/MI_W3D_<Opaque|Masked|Alpha|Additive|Multiply>[_2S]
  Materials/M_Particle, MI_Particle_<Additive|Alpha|Masked|Multiply>   particle / tracer / scorch quads
  Materials/M_Terrain                      3-layer atlas blend used by the terrain
  Materials/M_Shroud                       post process: the fog of war darkens the scene by world position
  Materials/M_Water, M_River               standing water (opacity by depth, per vertex) and rivers
  Materials/M_Road                         roads: lit like the terrain, the texture's alpha as a dithered mask
  Maps/Skirmish                            empty level; the game mode builds everything

Material parameters (set at runtime by UGenAssetSubsystem / UGenTerrainComponent):
  BaseTexture, UseTexture, Tint (rgb * diffuse, a = opacity), Emissive, HouseColor, HouseColorMode,
  HouseColorShape (1 for an alpha-blended house-coloured quad: the house-colour mask is its shape)
  EnvTexture, UnderTint, UnderlayMode (opaque first pass under an alpha-blended texture, see W3D_SHADE_HLSL)
  IgnoreAlpha (an opaque section in the masked material), and per component, as custom primitive data:
  [0] StealthFade (0 solid .. 1 gone), [1] StealthGlow (0 .. 1: the look of a detected stealth unit),
  [2..4] ObjectTint (light added to the object: the white flash of something just selected),
  [5..7] TreadLeft / TreadRight / TreadMiddle (how far a tank's treads have rolled; ScrollMode 2..4)
  ScrollMode, UVScroll (UPerSec, VPerSec, UOffset, VOffset), UVScale: stage 0 LinearOffsetTextureMapperClass
  Atlas (terrain)
  Shroud, ShroudScale, ShroudOffset (fog of war: one texel per cell, and the world-to-texture mapping)
  WaterTexture, SparkleTexture, NoiseTexture, EdgeTexture (rivers), WaterTint, SparkleScale
Re-running is safe: existing assets are rebuilt in place.
"""
import unreal

MEL = unreal.MaterialEditingLibrary
ATL = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary

ROOT = "/Game/Generals"
MAT_DIR = ROOT + "/Materials"
MAP_DIR = ROOT + "/Maps"

# Stage 0 texture coordinates. LinearOffsetTextureMapperClass: u' = u * UScale + offset, the offset sliding by
# -UPerSec per second from UOffset (wrapped to 0..1); ScrollMode 1. Tank treads (ScrollMode 2..4, left / right /
# middle): the offset their draw module has rolled them to. The mesh's V was flipped on import (1 - v).
W3D_UV_HLSL = r"""
float2 o = float2(0.0, 0.0);
if (ScrollMode > 0.5 && ScrollMode < 1.5)
{
    o = frac(-UVScroll.xy * Clock + UVScroll.zw);
}
else if (ScrollMode > 1.5)
{
    o = float2(ScrollMode < 2.5 ? TreadL : (ScrollMode < 3.5 ? TreadR : TreadM), 0.0);
}
float u = UV.x * UVScale.x + o.x;
float v = (1.0 - UV.y) * UVScale.y + o.y;
return float2(u, 1.0 - v);
"""

# W3D shading, mirroring the original fixed-function pipeline plus Generals house color:
#   HouseColorMode 1: mesh named HOUSECOLOR* -> diffuse multiplied by the player color
#   HouseColorMode 2: ZHC* texture -> pixels with alpha < 1 get the player's hue/saturation
#                     (W3DAssetManager remapAlphaTexture16Bit, DO_HUE_SHIFT), alpha forced opaque,
#                     unless HouseColorShape: for an alpha-blended house-coloured quad (a logo painted on
#                     the ground) the mask is the shape and everything outside it is left transparent.
W3D_SHADE_HLSL = r"""
float3 c = (UseTexture > 0.5) ? Tex.rgb : float3(1, 1, 1);
float a = (UseTexture > 0.5) ? Tex.a : 1.0;
if (HouseColorMode > 1.5)
{
    float mask = 1.0 - a;   // the ZHC texture's alpha channel is the house-colour intensity
    if (mask > 0.03)
    {
        float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
        float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
        float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
        float d = q.x - min(q.w, q.y);
        float3 hsv = float3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
        float3 h = HouseColor;
        float4 p2 = lerp(float4(h.bg, K.wz), float4(h.gb, K.xy), step(h.b, h.g));
        float4 q2 = lerp(float4(p2.xyw, h.r), float4(h.r, p2.yzx), step(p2.x, h.r));
        float d2 = q2.x - min(q2.w, q2.y);
        float3 hhsv = float3(abs(q2.z + (q2.w - q2.y) / (6.0 * d2 + 1e-10)), d2 / (q2.x + 1e-10), q2.x);
        hsv.x = hhsv.x;
        hsv.y *= hhsv.y;
        float4 K2 = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
        float3 pp = abs(frac(hsv.xxx + K2.xyz) * 6.0 - K2.www);
        c = hsv.z * lerp(K2.xxx, saturate(pp - K2.xxx), hsv.y);
    }
    // The original forces a house-coloured texture opaque. A house-coloured quad drawn with alpha
    // blending is the logo painted on the ground in front of a command center: there the mask is the
    // shape itself (the base art that goes with it has exactly that shape as its alpha), so the painted
    // part is drawn opaque and everything else leaves the ground showing through.
    a = (HouseColorShape > 0.5) ? ((mask > 0.03) ? 1.0 : 0.0) : 1.0;
    c *= TintRGB;
}
else if (HouseColorMode > 0.5)
{
    // Recolor_Vertex_Material: the mesh's (placeholder red) diffuse/ambient are replaced by the house color.
    c *= HouseColor;
}
else
{
    c *= TintRGB;
}
c *= VColorRGB;
a *= TintA * VColorA;
if (UnderlayMode > 0.5)
{
    // Two-pass "W3D Reflectivity" mesh: an opaque first pass (environment map or plain color) with this
    // texture alpha-blended over it, so the first pass shows where the texture's alpha is below 1.
    c = lerp(Under * UnderTint, c, saturate(a));
    a = 1.0;
}
if (IgnoreAlpha > 0.5)
{
    // An opaque section drawn through the masked material so that it can fade: its alpha means nothing.
    a = 1.0;
}
if (StealthGlow > 0.001)
{
    // A stealthed unit that has been detected (the original's heat-vision material pass).
    c = lerp(c, float3(1.0, 0.22, 0.08), StealthGlow);
}
if (StealthFade > 0.001)
{
    // Stealth: the model thins out through a screen-door pattern that changes every frame, which the
    // temporal anti-aliasing blends into translucency (the lighting stays that of the solid model).
    float n = InterleavedGradientNoise(Parameters.SvPosition.xy, ResolvedView.StateFrameIndexMod8);
    a *= (n >= StealthFade) ? 1.0 : 0.0;
}
return float4(c, a);
"""


MPC_PATH = MAT_DIR + "/MPC_GenLighting"


def build_lighting_collection():
    """Global lighting values set from the map's GlobalLighting chunk at load time."""
    if EAL.does_asset_exist(MPC_PATH):
        mpc = EAL.load_asset(MPC_PATH)
        # Materials refer to the collection's parameters by id, and setting the list anew gives them new
        # ids: every material built earlier (and not rebuilt in this run) would stop compiling. So an
        # existing collection that already has the parameters is left exactly as it is.
        names = [str(p.get_editor_property("parameter_name")) for p in mpc.get_editor_property("vector_parameters")]
        if "TerrainAmbient" in names and "ObjectAmbient" in names:
            return mpc
    else:
        mpc = ATL.create_asset("MPC_GenLighting", MAT_DIR, unreal.MaterialParameterCollection,
                               unreal.MaterialParameterCollectionFactoryNew())
    params = []
    for name, val in (("TerrainAmbient", (0.3, 0.3, 0.3, 1)), ("ObjectAmbient", (0.3, 0.3, 0.3, 1))):
        p = unreal.CollectionVectorParameter()
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", unreal.LinearColor(*val))
        params.append(p)
    mpc.set_editor_property("vector_parameters", params)
    EAL.save_loaded_asset(mpc)
    return mpc


def collection_param(mat, mpc, name, x, y):
    e = expr(mat, unreal.MaterialExpressionCollectionParameter, x, y)
    e.set_editor_property("collection", mpc)
    e.set_editor_property("parameter_name", name)
    return e


def ensure_dir(path):
    if not EAL.does_directory_exist(path):
        EAL.make_directory(path)


def recreate_material(name):
    path = f"{MAT_DIR}/{name}"
    if EAL.does_asset_exist(path):
        mat = EAL.load_asset(path)
        MEL.delete_all_material_expressions(mat)
        return mat
    return ATL.create_asset(name, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())


def expr(mat, cls, x, y):
    return MEL.create_material_expression(mat, cls, x, y)


def vector_param(mat, name, default, x, y):
    e = expr(mat, unreal.MaterialExpressionVectorParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", unreal.LinearColor(*default))
    return e


def scalar_param(mat, name, default, x, y):
    e = expr(mat, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", default)
    return e


def mask(mat, src, src_out, rgba, x, y):
    m = expr(mat, unreal.MaterialExpressionComponentMask, x, y)
    for ch, on in zip("rgba", rgba):
        m.set_editor_property(ch, on)
    MEL.connect_material_expressions(src, src_out, m, "")
    return m


def build_w3d_material(name, lit, mpc=None):
    mat = recreate_material(name)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE if lit else unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT if lit else unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("opacity_mask_clip_value", 0.375)  # W3D alpha test ref 0x60

    tex = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, -200)
    tex.set_editor_property("parameter_name", "BaseTexture")
    white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if white:
        tex.set_editor_property("texture", white)

    # Stage 0 coordinates (W3D_UV_HLSL): LinearOffsetTextureMapperClass slides them with time; a tank's treads
    # by the offsets its draw module rolls them to (custom primitive data 5..7, UGenModelComponent::SetTreadOffsets).
    uv0 = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1500, -700)
    clock = expr(mat, unreal.MaterialExpressionTime, -1500, -600)
    scroll_mode = scalar_param(mat, "ScrollMode", 0.0, -1500, -500)
    uv_scroll = vector_param(mat, "UVScroll", (0, 0, 0, 0), -1500, -400)
    uv_scale = vector_param(mat, "UVScale", (1, 1, 0, 0), -1500, -250)
    treads = []
    for i, n in enumerate(("TreadLeft", "TreadRight", "TreadMiddle")):
        p = scalar_param(mat, n, 0.0, -1800, -500 + i * 120)
        p.set_editor_property("use_custom_primitive_data", True)
        p.set_editor_property("primitive_data_index", 5 + i)
        treads.append(p)
    uv_node = expr(mat, unreal.MaterialExpressionCustom, -1150, -400)
    uv_node.set_editor_property("code", W3D_UV_HLSL)
    uv_node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    uv_node.set_editor_property("description", "W3DUV")
    uv_inputs = []
    for n in ("UV", "Clock", "ScrollMode", "UVScroll", "UVScale", "TreadL", "TreadR", "TreadM"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        uv_inputs.append(ci)
    uv_node.set_editor_property("inputs", uv_inputs)
    MEL.connect_material_expressions(uv0, "", uv_node, "UV")
    MEL.connect_material_expressions(clock, "", uv_node, "Clock")
    MEL.connect_material_expressions(scroll_mode, "", uv_node, "ScrollMode")
    MEL.connect_material_expressions(uv_scroll, "RGBA", uv_node, "UVScroll")
    MEL.connect_material_expressions(uv_scale, "RGBA", uv_node, "UVScale")
    MEL.connect_material_expressions(treads[0], "", uv_node, "TreadL")
    MEL.connect_material_expressions(treads[1], "", uv_node, "TreadR")
    MEL.connect_material_expressions(treads[2], "", uv_node, "TreadM")
    MEL.connect_material_expressions(uv_node, "", tex, "UVs")

    use_tex = scalar_param(mat, "UseTexture", 1.0, -900, 50)
    tint = vector_param(mat, "Tint", (1, 1, 1, 1), -900, 150)
    house = vector_param(mat, "HouseColor", (1, 1, 1, 1), -900, 350)
    house_mode = scalar_param(mat, "HouseColorMode", 0.0, -900, 550)
    house_shape = scalar_param(mat, "HouseColorShape", 0.0, -900, 750)
    emissive = vector_param(mat, "Emissive", (0, 0, 0, 0), -900, 650)
    vcol = expr(mat, unreal.MaterialExpressionVertexColor, -900, 850)
    ignore_alpha = scalar_param(mat, "IgnoreAlpha", 0.0, -1300, 50)
    # Per component (custom primitive data; 0 unless the component sets it): see UGenModelComponent::SetStealthLook.
    stealth_fade = scalar_param(mat, "StealthFade", 0.0, -1300, 200)
    stealth_fade.set_editor_property("use_custom_primitive_data", True)
    stealth_fade.set_editor_property("primitive_data_index", 0)
    stealth_glow = scalar_param(mat, "StealthGlow", 0.0, -1300, 350)
    stealth_glow.set_editor_property("use_custom_primitive_data", True)
    stealth_glow.set_editor_property("primitive_data_index", 1)

    if lit:
        # First pass of two-pass "W3D Reflectivity" meshes: an environment map looked up with the camera-space
        # reflection vector (EnvironmentMapperClass: uv = R.xy * 0.5 + 0.5), tinted by that pass's diffuse color.
        refl = expr(mat, unreal.MaterialExpressionReflectionVectorWS, -1900, 1200)
        to_view = expr(mat, unreal.MaterialExpressionTransform, -1700, 1200)
        to_view.set_editor_property("transform_source_type", unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
        to_view.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_VIEW)
        MEL.connect_material_expressions(refl, "", to_view, "")
        refl_xy = mask(mat, to_view, "", (True, True, False, False), -1500, 1200)
        half = expr(mat, unreal.MaterialExpressionMultiply, -1350, 1200)
        half.set_editor_property("const_b", 0.5)
        MEL.connect_material_expressions(refl_xy, "", half, "A")
        env_uv = expr(mat, unreal.MaterialExpressionAdd, -1200, 1200)
        env_uv.set_editor_property("const_b", 0.5)
        MEL.connect_material_expressions(half, "", env_uv, "A")
        under = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1000, 1200)
        under.set_editor_property("parameter_name", "EnvTexture")
        if white:
            under.set_editor_property("texture", white)
        MEL.connect_material_expressions(env_uv, "", under, "UVs")
        under_out = "RGB"
        under_tint = vector_param(mat, "UnderTint", (1, 1, 1, 1), -900, 1450)
        underlay_mode = scalar_param(mat, "UnderlayMode", 0.0, -900, 1650)
    else:
        # Unlit (additive / multiply) sections never have an underlay; constants let the compiler drop it.
        under = expr(mat, unreal.MaterialExpressionConstant3Vector, -900, 1200)
        under.set_editor_property("constant", unreal.LinearColor(1, 1, 1, 1))
        under_out = ""
        under_tint = under
        underlay_mode = expr(mat, unreal.MaterialExpressionConstant, -900, 1450)
        underlay_mode.set_editor_property("r", 0.0)

    custom = expr(mat, unreal.MaterialExpressionCustom, -500, 0)
    custom.set_editor_property("code", W3D_SHADE_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    custom.set_editor_property("description", "W3DShade")
    inputs = []
    for n in ("Tex", "UseTexture", "TintRGB", "TintA", "HouseColor", "HouseColorMode", "HouseColorShape",
              "VColorRGB", "VColorA",
              "Under", "UnderTint", "UnderlayMode", "IgnoreAlpha", "StealthFade", "StealthGlow"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    MEL.connect_material_expressions(under, under_out, custom, "Under")
    MEL.connect_material_expressions(under_tint, "", custom, "UnderTint")
    MEL.connect_material_expressions(underlay_mode, "", custom, "UnderlayMode")
    MEL.connect_material_expressions(tex, "RGBA", custom, "Tex")
    MEL.connect_material_expressions(use_tex, "", custom, "UseTexture")
    MEL.connect_material_expressions(tint, "", custom, "TintRGB")
    MEL.connect_material_expressions(tint, "A", custom, "TintA")
    MEL.connect_material_expressions(house, "", custom, "HouseColor")
    MEL.connect_material_expressions(house_mode, "", custom, "HouseColorMode")
    MEL.connect_material_expressions(house_shape, "", custom, "HouseColorShape")
    MEL.connect_material_expressions(vcol, "", custom, "VColorRGB")
    MEL.connect_material_expressions(vcol, "A", custom, "VColorA")
    MEL.connect_material_expressions(ignore_alpha, "", custom, "IgnoreAlpha")
    MEL.connect_material_expressions(stealth_fade, "", custom, "StealthFade")
    MEL.connect_material_expressions(stealth_glow, "", custom, "StealthGlow")

    rgb = mask(mat, custom, "", (True, True, True, False), -250, 0)
    alpha = mask(mat, custom, "", (False, False, False, True), -250, 150)
    emissive_rgb = mask(mat, emissive, "", (True, True, True, False), -500, 400)

    if lit:
        MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_BASE_COLOR)
        # Emissive = color * (material emissive + scene ambient): W3D vertex lighting's ambient term.
        amb = collection_param(mat, mpc, "ObjectAmbient", -900, 1000)
        amb_rgb = mask(mat, amb, "", (True, True, True, False), -650, 1000)
        add = expr(mat, unreal.MaterialExpressionAdd, -400, 500)
        MEL.connect_material_expressions(emissive_rgb, "", add, "A")
        MEL.connect_material_expressions(amb_rgb, "", add, "B")
        # ... plus the object's own tint (custom primitive data 2..4; see UGenModelComponent::SetTint): RTS3DScene
        # adds a drawable's tint - the white flash of something just selected - to the light it is drawn with.
        obj_tint = vector_param(mat, "ObjectTint", (0, 0, 0, 0), -1300, 700)
        obj_tint.set_editor_property("use_custom_primitive_data", True)
        obj_tint.set_editor_property("primitive_data_index", 2)
        tint_rgb = mask(mat, obj_tint, "", (True, True, True, False), -1100, 700)
        add_tint = expr(mat, unreal.MaterialExpressionAdd, -330, 620)
        MEL.connect_material_expressions(add, "", add_tint, "A")
        MEL.connect_material_expressions(tint_rgb, "", add_tint, "B")
        em = expr(mat, unreal.MaterialExpressionMultiply, -250, 400)
        MEL.connect_material_expressions(add_tint, "", em, "A")
        MEL.connect_material_expressions(rgb, "", em, "B")
        MEL.connect_material_property(em, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        rough = expr(mat, unreal.MaterialExpressionConstant, -250, 550)
        rough.set_editor_property("r", 0.85)
        MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
        spec = expr(mat, unreal.MaterialExpressionConstant, -250, 650)
        spec.set_editor_property("r", 0.25)
        MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    else:
        MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    return mat


def make_instance(name, parent, blend, two_sided):
    path = f"{MAT_DIR}/{name}"
    if EAL.does_asset_exist(path):
        mi = EAL.load_asset(path)
    else:
        mi = ATL.create_asset(name, MAT_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    mi.set_editor_property("parent", parent)
    ov = unreal.MaterialInstanceBasePropertyOverrides()
    ov.set_editor_property("override_blend_mode", True)
    ov.set_editor_property("blend_mode", blend)
    ov.set_editor_property("override_two_sided", True)
    ov.set_editor_property("two_sided", two_sided)
    ov.set_editor_property("override_opacity_mask_clip_value", True)
    ov.set_editor_property("opacity_mask_clip_value", 0.375)
    mi.set_editor_property("base_property_overrides", ov)
    MEL.update_material_instance(mi)
    EAL.save_loaded_asset(mi)


# Particle shading (W3D sprite shaders): color = texture * vertex color, alpha = texture alpha * vertex alpha.
# The original multiplied and blended in gamma space, and vertex colors arrive as gamma values, so the
# product is formed in gamma space (exact sRGB curve: its linear toe matters for dim glow texels).
# GammaAdd = 1 (additive particles) outputs that gamma product as-is, which adds up against mid-tone
# terrain the way the original's gamma-space addition did; otherwise it is converted back to linear.
# Exposure undoes the scene's exposure scale.
PARTICLE_HLSL = r"""
float3 t = max(Tex.rgb, 0.0);
float3 tg = lerp(1.055 * pow(max(t, 1e-7), 1.0 / 2.4) - 0.055, t * 12.92, step(t, 0.0031308));
float3 g = saturate(tg * VColorRGB);
float3 lin = lerp(pow((g + 0.055) / 1.055, 2.4), g / 12.92, step(g, 0.04045));
float3 c = lerp(lin, g, GammaAdd) * Exposure;
float a = lerp(1.0, Tex.a * VColorA, UseAlpha);
// Many sprite textures are not quite black / transparent at their borders, which shows as a faint
// rectangle on large glows; EdgeFade = 1 fades the outer 1/16 of each quad.
float2 e = min(UV, 1.0 - UV);
a *= lerp(1.0, saturate(min(e.x, e.y) * 16.0), EdgeFade);
return float4(c, a);
"""


def build_particle_material():
    mat = recreate_material("M_Particle")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("opacity_mask_clip_value", 0.375)

    tex = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, -200)
    tex.set_editor_property("parameter_name", "BaseTexture")
    white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if white:
        tex.set_editor_property("texture", white)
    use_alpha = scalar_param(mat, "UseAlpha", 1.0, -900, 50)
    gamma_add = scalar_param(mat, "GammaAdd", 0.0, -900, 150)
    exposure = scalar_param(mat, "Exposure", 1.2, -900, 250)
    vcol = expr(mat, unreal.MaterialExpressionVertexColor, -900, 400)

    custom = expr(mat, unreal.MaterialExpressionCustom, -500, 0)
    custom.set_editor_property("code", PARTICLE_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    custom.set_editor_property("description", "ParticleShade")
    inputs = []
    for n in ("Tex", "VColorRGB", "VColorA", "UseAlpha", "GammaAdd", "Exposure", "UV", "EdgeFade"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    uv = expr(mat, unreal.MaterialExpressionTextureCoordinate, -900, 750)
    edge_fade = scalar_param(mat, "EdgeFade", 1.0, -900, 850)
    MEL.connect_material_expressions(uv, "", custom, "UV")
    MEL.connect_material_expressions(edge_fade, "", custom, "EdgeFade")
    MEL.connect_material_expressions(tex, "RGBA", custom, "Tex")
    MEL.connect_material_expressions(vcol, "", custom, "VColorRGB")
    MEL.connect_material_expressions(vcol, "A", custom, "VColorA")
    MEL.connect_material_expressions(use_alpha, "", custom, "UseAlpha")
    MEL.connect_material_expressions(gamma_add, "", custom, "GammaAdd")
    MEL.connect_material_expressions(exposure, "", custom, "Exposure")

    rgb = mask(mat, custom, "", (True, True, True, False), -250, 0)
    alpha = mask(mat, custom, "", (False, False, False, True), -250, 150)

    # Soft particles: fade out where a sprite cuts into the terrain or a model instead of ending in a hard
    # line (the original's sprites did). SoftFade = 0 turns it off for quads that lie flat on the ground.
    soft = scalar_param(mat, "SoftFade", 1.0, -900, 550)
    fade_dist = scalar_param(mat, "SoftFadeDistance", 80.0, -900, 650)
    depth_fade = expr(mat, unreal.MaterialExpressionDepthFade, -650, 600)
    MEL.connect_material_expressions(fade_dist, "", depth_fade, "FadeDistance")
    one = expr(mat, unreal.MaterialExpressionConstant, -650, 500)
    one.set_editor_property("r", 1.0)
    fade = expr(mat, unreal.MaterialExpressionLinearInterpolate, -450, 550)
    MEL.connect_material_expressions(one, "", fade, "A")
    MEL.connect_material_expressions(depth_fade, "", fade, "B")
    MEL.connect_material_expressions(soft, "", fade, "Alpha")
    opacity = expr(mat, unreal.MaterialExpressionMultiply, -100, 300)
    MEL.connect_material_expressions(alpha, "", opacity, "A")
    MEL.connect_material_expressions(fade, "", opacity, "B")

    MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    return mat


CLOUD_UV_HLSL = r"""
// W3DShaderManager (TerrainShader2Stage::updateNoise1 / 2): the cloud and light map stages are mapped
// from the world's x / y, one repeat every 315 units (STRETCH_FACTOR: "covers 63/2 tiles"); the clouds
// slide by (-0.02, -0.03) of the texture a second. (Unreal's y is the game's mirrored, in centimetres.)
float2 g = float2(WP.x, -WP.y) * (0.1 / 315.0);
return float4(g + frac(T * float2(-0.02, -0.03)), g);
"""


def cloud_light_factor(mat, x, y):
    """The terrain shader's cloud and light map stages as one factor to multiply the colour by
    (fterrainnoise2.nvp: "modulate with texture 2", "modulate with texture 3"). CloudAmount and
    LightMapAmount (0 or 1) switch them, as UseCloudMap / UseLightMap and the time of day do."""
    wp = expr(mat, unreal.MaterialExpressionWorldPosition, x, y)
    time = expr(mat, unreal.MaterialExpressionTime, x, y + 120)
    uvs = custom_node(mat, CLOUD_UV_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT4, ("WP", "T"), x + 250, y, "CloudUV")
    MEL.connect_material_expressions(wp, "", uvs, "WP")
    MEL.connect_material_expressions(time, "", uvs, "T")
    cloud_uv = mask(mat, uvs, "", (True, True, False, False), x + 500, y)
    light_uv = mask(mat, uvs, "", (False, False, True, True), x + 500, y + 150)
    cloud = texture_param(mat, "CloudTexture", x + 700, y)
    MEL.connect_material_expressions(cloud_uv, "", cloud, "UVs")
    light = texture_param(mat, "LightMapTexture", x + 700, y + 250)
    MEL.connect_material_expressions(light_uv, "", light, "UVs")
    one = expr(mat, unreal.MaterialExpressionConstant, x + 700, y + 500)
    one.set_editor_property("r", 1.0)
    cloud_amt = scalar_param(mat, "CloudAmount", 0.0, x + 700, y + 600)
    light_amt = scalar_param(mat, "LightMapAmount", 0.0, x + 700, y + 700)
    cloud_f = expr(mat, unreal.MaterialExpressionLinearInterpolate, x + 950, y)
    MEL.connect_material_expressions(one, "", cloud_f, "A")
    MEL.connect_material_expressions(cloud, "RGB", cloud_f, "B")
    MEL.connect_material_expressions(cloud_amt, "", cloud_f, "Alpha")
    light_f = expr(mat, unreal.MaterialExpressionLinearInterpolate, x + 950, y + 250)
    MEL.connect_material_expressions(one, "", light_f, "A")
    MEL.connect_material_expressions(light, "RGB", light_f, "B")
    MEL.connect_material_expressions(light_amt, "", light_f, "Alpha")
    factor = expr(mat, unreal.MaterialExpressionMultiply, x + 1150, y + 100)
    MEL.connect_material_expressions(cloud_f, "", factor, "A")
    MEL.connect_material_expressions(light_f, "", factor, "B")
    return factor


def build_terrain_material(mpc):
    mat = recreate_material("M_Terrain")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    gray = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")

    samples = []
    for i in range(3):
        uv = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1100, -300 + i * 300)
        uv.set_editor_property("coordinate_index", i)
        s = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -850, -300 + i * 300)
        s.set_editor_property("parameter_name", "Atlas")
        if gray:
            s.set_editor_property("texture", gray)
        MEL.connect_material_expressions(uv, "", s, "UVs")
        samples.append(s)
    vcol = expr(mat, unreal.MaterialExpressionVertexColor, -850, 650)

    lerp1 = expr(mat, unreal.MaterialExpressionLinearInterpolate, -500, -150)
    MEL.connect_material_expressions(samples[0], "RGB", lerp1, "A")
    MEL.connect_material_expressions(samples[1], "RGB", lerp1, "B")
    MEL.connect_material_expressions(vcol, "R", lerp1, "Alpha")
    lerp2 = expr(mat, unreal.MaterialExpressionLinearInterpolate, -250, 0)
    MEL.connect_material_expressions(lerp1, "", lerp2, "A")
    MEL.connect_material_expressions(samples[2], "RGB", lerp2, "B")
    MEL.connect_material_expressions(vcol, "G", lerp2, "Alpha")
    factor = cloud_light_factor(mat, -2600, 1200)
    shaded = expr(mat, unreal.MaterialExpressionMultiply, -100, -100)
    MEL.connect_material_expressions(lerp2, "", shaded, "A")
    MEL.connect_material_expressions(factor, "", shaded, "B")
    MEL.connect_material_property(shaded, "", unreal.MaterialProperty.MP_BASE_COLOR)

    amb = collection_param(mat, mpc, "TerrainAmbient", -850, 900)
    amb_rgb = mask(mat, amb, "", (True, True, True, False), -600, 900)
    em = expr(mat, unreal.MaterialExpressionMultiply, -250, 200)
    MEL.connect_material_expressions(shaded, "", em, "A")
    MEL.connect_material_expressions(amb_rgb, "", em, "B")
    MEL.connect_material_property(em, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    rough = expr(mat, unreal.MaterialExpressionConstant, -250, 300)
    rough.set_editor_property("r", 0.95)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = expr(mat, unreal.MaterialExpressionConstant, -250, 400)
    spec.set_editor_property("r", 0.1)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def pick_enum(enum, names):
    for n in names:
        if hasattr(enum, n):
            return getattr(enum, n)
    raise RuntimeError("none of %s in %s: %s" % (names, enum, [n for n in dir(enum) if n.isupper()]))


def build_shroud_material():
    """Fog of war (W3DShroud): scene color * shroud texture, looked up by the pixel's world position.

    The texture holds the original's shroud levels (255 in view, 127 fogged, 0 unexplored), one texel per
    partition cell, bilinearly filtered. The original multiplies in gamma space, so the level is raised to
    2.2 before it meets the linear scene color. Runs before the tone mapper and before temporal AA (so the
    depth it reads matches the color), after translucency (so particles are dimmed too).
    """
    mat = recreate_material("M_Shroud")
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    mat.set_editor_property("blendable_location", pick_enum(
        unreal.BlendableLocation, ("BL_SCENE_COLOR_AFTER_DOF", "BL_BEFORE_TONEMAPPING")))

    wp = expr(mat, unreal.MaterialExpressionWorldPosition, -1300, 0)
    xy = mask(mat, wp, "", (True, True, False, False), -1100, 0)
    scale = vector_param(mat, "ShroudScale", (0.0, 0.0, 0.0, 0.0), -1300, 150)
    scale_xy = mask(mat, scale, "", (True, True, False, False), -1100, 150)
    offset = vector_param(mat, "ShroudOffset", (0.5, 0.5, 0.0, 0.0), -1300, 400)
    offset_xy = mask(mat, offset, "", (True, True, False, False), -1100, 400)
    mul = expr(mat, unreal.MaterialExpressionMultiply, -900, 50)
    MEL.connect_material_expressions(xy, "", mul, "A")
    MEL.connect_material_expressions(scale_xy, "", mul, "B")
    uv = expr(mat, unreal.MaterialExpressionAdd, -750, 150)
    MEL.connect_material_expressions(mul, "", uv, "A")
    MEL.connect_material_expressions(offset_xy, "", uv, "B")

    tex = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -550, 100)
    tex.set_editor_property("parameter_name", "Shroud")
    white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if white:
        tex.set_editor_property("texture", white)
    MEL.connect_material_expressions(uv, "", tex, "UVs")

    gamma = expr(mat, unreal.MaterialExpressionPower, -300, 100)
    gamma.set_editor_property("const_exponent", 2.2)
    MEL.connect_material_expressions(tex, "RGB", gamma, "Base")

    scene = expr(mat, unreal.MaterialExpressionSceneTexture, -550, -200)
    scene.set_editor_property("scene_texture_id", unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0)
    scene_rgb = mask(mat, scene, "Color", (True, True, True, False), -300, -200)

    out = expr(mat, unreal.MaterialExpressionMultiply, -100, 0)
    MEL.connect_material_expressions(scene_rgb, "", out, "A")
    MEL.connect_material_expressions(gamma, "", out, "B")
    MEL.connect_material_property(out, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    return mat


# Water (W3DWater.cpp). Positions are Unreal's: Generals x = X / 10, y = -Y / 10. The original's clock
# for all of this (m_riverVOrigin) moves 0.002 a frame at 30 frames a second.
WATER_UV_HLSL = r"""
// drawTrapezoidWater: the water texture repeats every 150 units and wobbles a little.
float gx = WP.x * 0.1;
float gy = -WP.y * 0.1;
float t = T * 0.06;
return float2(gx / 150.0 + 0.02 * cos(11.0 * t) * sin(25.0 * t + gx * 0.0785398),
              gy / 150.0 + 0.02 * cos(5.0 * t) * sin(25.0 * t + gy * 0.0785398));
"""
SPARKLE_UV_HLSL = r"""
// "white highlights on black background" every 50 units (BUMP_SIZE), sheared
float gx = WP.x * 0.1;
float gy = -WP.y * 0.1;
return float2(gx / 50.0, (gy + 0.3 * gx) / 50.0);
"""
NOISE_UV_HLSL = r"""
// the noise the highlights are multiplied with: every 16 units, drifting
float gx = WP.x * 0.1;
float gy = -WP.y * 0.1;
float t = T * 0.06;
return float2(gx / 16.0 + t, gy / 16.0 + t);
"""
RIVER_UV_HLSL = r"""
// drawRiverWater: v runs down the river, flowing and wobbling; U is 0 on one bank (0.5 / 1 on the other)
float t = T * 0.06;
return float2(UV.x, -t + UV.y + sin(6.2831853 * UV.y - 3.0 * t) / 22.0);
"""


def custom_node(mat, code, out_type, input_names, x, y, description):
    c = expr(mat, unreal.MaterialExpressionCustom, x, y)
    c.set_editor_property("code", code)
    c.set_editor_property("output_type", out_type)
    c.set_editor_property("description", description)
    inputs = []
    for n in input_names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    c.set_editor_property("inputs", inputs)
    return c


def texture_param(mat, name, x, y):
    t = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    t.set_editor_property("parameter_name", name)
    white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    if white:
        t.set_editor_property("texture", white)
    return t


def build_water_materials():
    """Standing water and rivers.

    Unlit: the game works the water's shade out itself, as the original does for a flat surface under the
    map's lights ("legacy lighting code"), and hands it over as WaterTint. (Unreal's lit translucency gets
    its light from a volume around the camera that a camera this high above the ground is outside of.)
    On top come the sparkles: two textures multiplied, one of them drifting.
      M_Water: opacity is the vertex alpha (the depth of the water at that terrain vertex).
      M_River: UV0 = (0 / 0.5 across, distance along), UV1 = (0 / 1 across, same); opacity and foam from
               the edge texture.
    """
    F2 = unreal.CustomMaterialOutputType.CMOT_FLOAT2
    for name in ("M_Water", "M_River"):
        river = name == "M_River"
        mat = recreate_material(name)
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        mat.set_editor_property("two_sided", True)

        wp = expr(mat, unreal.MaterialExpressionWorldPosition, -1500, 300)
        time = expr(mat, unreal.MaterialExpressionTime, -1500, 500)
        noise_uv = custom_node(mat, NOISE_UV_HLSL, F2, ("WP", "T"), -1200, 400, "NoiseUV")
        MEL.connect_material_expressions(wp, "", noise_uv, "WP")
        MEL.connect_material_expressions(time, "", noise_uv, "T")

        water = texture_param(mat, "WaterTexture", -850, -300)
        sparkle = texture_param(mat, "SparkleTexture", -850, 0)
        noise = texture_param(mat, "NoiseTexture", -850, 300)
        MEL.connect_material_expressions(noise_uv, "", noise, "UVs")
        edge = None
        if river:
            uv0 = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1500, -300)
            uv1 = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1500, -100)
            uv1.set_editor_property("coordinate_index", 1)
            flow = custom_node(mat, RIVER_UV_HLSL, F2, ("UV", "T"), -1200, -300, "RiverUV")
            MEL.connect_material_expressions(uv0, "", flow, "UV")
            MEL.connect_material_expressions(time, "", flow, "T")
            flow_edge = custom_node(mat, RIVER_UV_HLSL, F2, ("UV", "T"), -1200, -100, "RiverEdgeUV")
            MEL.connect_material_expressions(uv1, "", flow_edge, "UV")
            MEL.connect_material_expressions(time, "", flow_edge, "T")
            MEL.connect_material_expressions(flow, "", water, "UVs")
            MEL.connect_material_expressions(flow, "", sparkle, "UVs")
            edge = texture_param(mat, "EdgeTexture", -850, 600)
            MEL.connect_material_expressions(flow_edge, "", edge, "UVs")
        else:
            water_uv = custom_node(mat, WATER_UV_HLSL, F2, ("WP", "T"), -1200, -300, "WaterUV")
            MEL.connect_material_expressions(wp, "", water_uv, "WP")
            MEL.connect_material_expressions(time, "", water_uv, "T")
            sparkle_uv = custom_node(mat, SPARKLE_UV_HLSL, F2, ("WP",), -1200, 0, "SparkleUV")
            MEL.connect_material_expressions(wp, "", sparkle_uv, "WP")
            MEL.connect_material_expressions(water_uv, "", water, "UVs")
            MEL.connect_material_expressions(sparkle_uv, "", sparkle, "UVs")

        tint = vector_param(mat, "WaterTint", (1.0, 1.0, 1.0, 1.0), -850, -500)
        tint_rgb = mask(mat, tint, "", (True, True, True, False), -650, -500)
        sparkle_scale = scalar_param(mat, "SparkleScale", 1.2, -850, 1100)

        ambient = expr(mat, unreal.MaterialExpressionMultiply, -450, -350)
        MEL.connect_material_expressions(water, "RGB", ambient, "A")
        MEL.connect_material_expressions(tint_rgb, "", ambient, "B")
        glint = expr(mat, unreal.MaterialExpressionMultiply, -450, 100)
        MEL.connect_material_expressions(sparkle, "RGB", glint, "A")
        MEL.connect_material_expressions(noise, "RGB", glint, "B")
        extra = glint
        if river:
            # ("add r0.rgb, r0, t3": the edge texture's foam specks go on top)
            foam = expr(mat, unreal.MaterialExpressionAdd, -250, 250)
            MEL.connect_material_expressions(glint, "", foam, "A")
            MEL.connect_material_expressions(edge, "RGB", foam, "B")
            extra = foam
        extra_scaled = expr(mat, unreal.MaterialExpressionMultiply, -100, 150)
        MEL.connect_material_expressions(extra, "", extra_scaled, "A")
        MEL.connect_material_expressions(sparkle_scale, "", extra_scaled, "B")
        emissive = expr(mat, unreal.MaterialExpressionAdd, 50, -100)
        MEL.connect_material_expressions(ambient, "", emissive, "A")
        MEL.connect_material_expressions(extra_scaled, "", emissive, "B")
        MEL.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

        if river:
            MEL.connect_material_property(edge, "A", unreal.MaterialProperty.MP_OPACITY)
        else:
            vcol = expr(mat, unreal.MaterialExpressionVertexColor, -450, 500)
            MEL.connect_material_property(vcol, "A", unreal.MaterialProperty.MP_OPACITY)
        MEL.recompile_material(mat)
        EAL.save_loaded_asset(mat)


ROAD_MASK_HLSL = r"""
// The texture's alpha as coverage: each pixel is kept when the alpha beats a noise value that changes
// from pixel to pixel and from frame to frame, which temporal anti-aliasing then averages into a blend.
float n = InterleavedGradientNoise(Parameters.SvPosition.xy, View.StateFrameIndexMod8);
return A - n + 0.499;
"""


def build_road_material(mpc):
    """Roads (W3DRoadBuffer): a road type's texture on a mesh that lies on the terrain.

    Opaque and lit exactly like the terrain under it (base colour under the map's lights, the terrain's
    ambient term as emissive), so shadows fall on roads as on the ground. The original blends the road
    texture by its alpha; here the alpha is a dithered mask (clip value 0.5).
    """
    mat = recreate_material("M_Road")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("opacity_mask_clip_value", 0.5)

    tex = texture_param(mat, "RoadTexture", -850, -200)
    # roadnoise2.nvp: "modulate with cloud", "modulate with noise".
    factor = cloud_light_factor(mat, -2600, 600)
    shaded = expr(mat, unreal.MaterialExpressionMultiply, -500, -200)
    MEL.connect_material_expressions(tex, "RGB", shaded, "A")
    MEL.connect_material_expressions(factor, "", shaded, "B")
    MEL.connect_material_property(shaded, "", unreal.MaterialProperty.MP_BASE_COLOR)

    amb = collection_param(mat, mpc, "TerrainAmbient", -850, 200)
    amb_rgb = mask(mat, amb, "", (True, True, True, False), -600, 200)
    em = expr(mat, unreal.MaterialExpressionMultiply, -350, 100)
    MEL.connect_material_expressions(shaded, "", em, "A")
    MEL.connect_material_expressions(amb_rgb, "", em, "B")
    MEL.connect_material_property(em, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    dither = custom_node(mat, ROAD_MASK_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT1, ("A",), -350, -350, "RoadMask")
    MEL.connect_material_expressions(tex, "A", dither, "A")
    MEL.connect_material_property(dither, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    rough = expr(mat, unreal.MaterialExpressionConstant, -350, 300)
    rough.set_editor_property("r", 0.95)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = expr(mat, unreal.MaterialExpressionConstant, -350, 400)
    spec.set_editor_property("r", 0.1)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def build_level():
    """Empty level holding one GenMapActor (use its 'Load Map' button to preview maps in the editor)."""
    path = f"{MAP_DIR}/Skirmish"
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if EAL.does_asset_exist(path):
        les.load_level(path)
    else:
        les.new_level(path)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not any(isinstance(a, unreal.GenMapActor) for a in eas.get_all_level_actors()):
        actor = eas.spawn_actor_from_class(unreal.GenMapActor, unreal.Vector(0, 0, 0))
        actor.set_actor_label("GeneralsMap")
    les.save_current_level()


def build_sky_material():
    # The sky box (W3DWater's new_skybox.w3d): its material is all emissive, so the pictures show as they are,
    # whatever the map's light. Opaque, unlit, seen from inside.
    mat = recreate_material("M_Sky")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    tex = texture_param(mat, "SkyTexture", -600, 0)
    MEL.connect_material_property(tex, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def build_decal_material():
    # The pictures laid on the ground round a spot (RadiusDecal: the radius cursors of powers being aimed, a
    # gunship's target area). All of them are SHADOW_ALPHA_DECALs: the texture times Color, its alpha times
    # Opacity (which the game throbs). Glow lifts it out of the lighting a little, as the original drew it unlit.
    mat = recreate_material("M_RadiusDecal")
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    tex = texture_param(mat, "DecalTexture", -900, -200)
    color = vector_param(mat, "Color", (1, 1, 1, 1), -900, 100)
    color_rgb = mask(mat, color, "", (True, True, True, False), -700, 100)
    opacity = scalar_param(mat, "Opacity", 1.0, -900, 250)
    glow = scalar_param(mat, "Glow", 0.6, -900, 350)
    rgb = expr(mat, unreal.MaterialExpressionMultiply, -500, -100)
    MEL.connect_material_expressions(tex, "RGB", rgb, "A")
    MEL.connect_material_expressions(color_rgb, "", rgb, "B")
    em = expr(mat, unreal.MaterialExpressionMultiply, -300, 0)
    MEL.connect_material_expressions(rgb, "", em, "A")
    MEL.connect_material_expressions(glow, "", em, "B")
    alpha = expr(mat, unreal.MaterialExpressionMultiply, -500, 200)
    MEL.connect_material_expressions(tex, "A", alpha, "A")
    MEL.connect_material_expressions(opacity, "", alpha, "B")
    MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(em, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)


def main():
    ensure_dir(ROOT)
    ensure_dir(MAT_DIR)
    ensure_dir(MAP_DIR)

    # GEN_SETUP_ONLY=shroud rebuilds just that material (the rest takes a while to recompile).
    import os
    only = os.environ.get("GEN_SETUP_ONLY", "")
    if only == "shroud":
        build_shroud_material()
        unreal.log("Generals setup complete (shroud material only)")
        return
    if only == "water":
        build_water_materials()
        unreal.log("Generals setup complete (water materials only)")
        return
    if only == "terrain":
        mpc = build_lighting_collection()
        build_terrain_material(mpc)
        build_road_material(mpc)
        unreal.log("Generals setup complete (terrain and road materials only)")
        return
    if only == "sky":
        build_sky_material()
        unreal.log("Generals setup complete (sky material only)")
        return
    if only == "decal":
        build_decal_material()
        unreal.log("Generals setup complete (decal material only)")
        return
    if only == "roads":
        build_road_material(build_lighting_collection())
        unreal.log("Generals setup complete (road material only)")
        return

    mpc = build_lighting_collection()
    lit = build_w3d_material("M_W3D_Lit", True, mpc)
    unlit = build_w3d_material("M_W3D_Unlit", False, mpc)
    B = unreal.BlendMode
    for two_sided in (False, True):
        sfx = "_2S" if two_sided else ""
        make_instance("MI_W3D_Opaque" + sfx, lit, B.BLEND_OPAQUE, two_sided)
        make_instance("MI_W3D_Masked" + sfx, lit, B.BLEND_MASKED, two_sided)
        make_instance("MI_W3D_Alpha" + sfx, lit, B.BLEND_TRANSLUCENT, two_sided)
        make_instance("MI_W3D_Additive" + sfx, unlit, B.BLEND_ADDITIVE, two_sided)
        make_instance("MI_W3D_Multiply" + sfx, unlit, B.BLEND_MODULATE, two_sided)
    particle = build_particle_material()
    make_instance("MI_Particle_Additive", particle, B.BLEND_ADDITIVE, True)
    make_instance("MI_Particle_Alpha", particle, B.BLEND_TRANSLUCENT, True)
    make_instance("MI_Particle_Masked", particle, B.BLEND_MASKED, True)
    make_instance("MI_Particle_Multiply", particle, B.BLEND_MODULATE, True)
    if only == "w3d":
        unreal.log("Generals setup complete (model and particle materials only)")
        return
    build_terrain_material(mpc)
    build_shroud_material()
    build_water_materials()
    build_road_material(mpc)
    build_decal_material()
    build_sky_material()
    build_level()
    unreal.log("Generals setup complete")


main()
