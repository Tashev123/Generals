"""
One-time editor setup for the Generals recreation (run inside Unreal's Python):

  UnrealEditor-Cmd.exe Generals1.uproject -run=pythonscript -script="<path>/ue_setup.py"

Creates under /Game/Generals:
  Materials/M_W3D_Lit, M_W3D_Unlit         base materials for W3D meshes
  Materials/MI_W3D_<Opaque|Masked|Alpha|Additive|Multiply>[_2S]
  Materials/M_Particle, MI_Particle_<Additive|Alpha|Masked|Multiply>   particle / tracer / scorch quads
  Materials/M_Terrain                      3-layer atlas blend used by the terrain
  Maps/Skirmish                            empty level; the game mode builds everything

Material parameters (set at runtime by UGenAssetSubsystem / UGenTerrainComponent):
  BaseTexture, UseTexture, Tint (rgb * diffuse, a = opacity), Emissive, HouseColor, HouseColorMode
  Atlas (terrain)
Re-running is safe: existing assets are rebuilt in place.
"""
import unreal

MEL = unreal.MaterialEditingLibrary
ATL = unreal.AssetToolsHelpers.get_asset_tools()
EAL = unreal.EditorAssetLibrary

ROOT = "/Game/Generals"
MAT_DIR = ROOT + "/Materials"
MAP_DIR = ROOT + "/Maps"

# W3D shading, mirroring the original fixed-function pipeline plus Generals house color:
#   HouseColorMode 1: mesh named HOUSECOLOR* -> diffuse multiplied by the player color
#   HouseColorMode 2: ZHC* texture -> pixels with alpha < 1 get the player's hue/saturation
#                     (W3DAssetManager remapAlphaTexture16Bit, DO_HUE_SHIFT), alpha forced opaque
W3D_SHADE_HLSL = r"""
float3 c = (UseTexture > 0.5) ? Tex.rgb : float3(1, 1, 1);
float a = (UseTexture > 0.5) ? Tex.a : 1.0;
if (HouseColorMode > 1.5)
{
    if (a < 0.97)
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
    a = 1.0;
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
return float4(c, a * TintA * VColorA);
"""


MPC_PATH = MAT_DIR + "/MPC_GenLighting"


def build_lighting_collection():
    """Global lighting values set from the map's GlobalLighting chunk at load time."""
    if EAL.does_asset_exist(MPC_PATH):
        mpc = EAL.load_asset(MPC_PATH)
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

    use_tex = scalar_param(mat, "UseTexture", 1.0, -900, 50)
    tint = vector_param(mat, "Tint", (1, 1, 1, 1), -900, 150)
    house = vector_param(mat, "HouseColor", (1, 1, 1, 1), -900, 350)
    house_mode = scalar_param(mat, "HouseColorMode", 0.0, -900, 550)
    emissive = vector_param(mat, "Emissive", (0, 0, 0, 0), -900, 650)
    vcol = expr(mat, unreal.MaterialExpressionVertexColor, -900, 850)

    custom = expr(mat, unreal.MaterialExpressionCustom, -500, 0)
    custom.set_editor_property("code", W3D_SHADE_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    custom.set_editor_property("description", "W3DShade")
    inputs = []
    for n in ("Tex", "UseTexture", "TintRGB", "TintA", "HouseColor", "HouseColorMode", "VColorRGB", "VColorA"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    MEL.connect_material_expressions(tex, "RGBA", custom, "Tex")
    MEL.connect_material_expressions(use_tex, "", custom, "UseTexture")
    MEL.connect_material_expressions(tint, "", custom, "TintRGB")
    MEL.connect_material_expressions(tint, "A", custom, "TintA")
    MEL.connect_material_expressions(house, "", custom, "HouseColor")
    MEL.connect_material_expressions(house_mode, "", custom, "HouseColorMode")
    MEL.connect_material_expressions(vcol, "", custom, "VColorRGB")
    MEL.connect_material_expressions(vcol, "A", custom, "VColorA")

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
        em = expr(mat, unreal.MaterialExpressionMultiply, -250, 400)
        MEL.connect_material_expressions(add, "", em, "A")
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
    MEL.connect_material_property(lerp2, "", unreal.MaterialProperty.MP_BASE_COLOR)

    amb = collection_param(mat, mpc, "TerrainAmbient", -850, 900)
    amb_rgb = mask(mat, amb, "", (True, True, True, False), -600, 900)
    em = expr(mat, unreal.MaterialExpressionMultiply, -250, 200)
    MEL.connect_material_expressions(lerp2, "", em, "A")
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


def main():
    ensure_dir(ROOT)
    ensure_dir(MAT_DIR)
    ensure_dir(MAP_DIR)

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
    build_terrain_material(mpc)
    build_level()
    unreal.log("Generals setup complete")


main()
