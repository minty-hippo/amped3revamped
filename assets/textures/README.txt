Amped 3 texture pack
====================

With the native renderer, any PNG in this folder (or a subfolder) named

    <anything>_<hash>.png        or        <hash>.png

replaces the game texture with that hash, at any size: the game uses
fractions of a texture, so an image 4x as large (same layout) drops in and
is drawn at full detail. <hash> is 16 hex digits: FNV-1a 64 over the
texture's base level as the game stores it (its DXT blocks, still tiled),
the same for every copy of the game. Only DXT-compressed 2D textures (all
of the game's menus and HUD) can be replaced.

The build also copies the project's assets/local/textures here (your own
replacements, kept out of the published sources), e.g. the new logo:

    AmpedBoxLogo_3B66B93CF489E918.png    the title screen logo (512x256 -> 2048x1024)
    LoadingScreen_4F1CA4AEA190FC18.png   the start-up loading screen (1024x512 -> 4096x2048)

made from assets/local/amped3_logo.png by tools/textures/make_logo_textures.py.

Separately, the game's fonts are rebuilt at 4x when first shown (nothing is
written here); a PNG here for a font's hash takes precedence. To draw a font
yourself, tools/textures/font_templates.py makes a guide and map for each.

The upscaled\ subfolder belongs to the texture upscaler ("Upscale
Textures.bat"); it's used last, only for textures nothing here replaces.
Don't put your own files there (a rerun with -Force rewrites it).

Order: your PNGs here > Sharp fonts > upscaled\ > the game's own.

Settings (amped3.toml, [GPU]):
    native_texture_pack = false        leave the game's textures as they are
    native_sharp_fonts = false         the game's own fonts
    native_upscaled_textures = false   don't use upscaled\

Each replacement used is noted in logs\amped3_NNN.log ("replaced texture").
