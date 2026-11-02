#!/usr/bin/env python3
# Hand resolutions for the merge conflicts left by merge.py (Russian translation + 60 FPS).
# Each rule replaces the n-th remaining conflict hunk of a file with explicit text.
# Written for 60 FPS Overhaul v2.0.2 + LazyDesman 5.0.8 on DELTARUNE V23: a rule that no longer
# matches fails loudly, and new conflicts need new rules.
import glob, re, sys

M = sys.argv[1]  # .../m
HUNK = re.compile(r'^<<<<<<< ru\n(.*?)^\|\|\|\|\|\|\| vanilla\n(.*?)^=======\n(.*?)^>>>>>>> 60fps\n', re.M | re.S)

GAMEPAD = ('for (var _gp_prime = 0; _gp_prime < 12; _gp_prime++)\n{\n'
           '    gamepad_is_connected(_gp_prime);\n}\n')

def ours_plus_gamepad(o, b, t):
    assert 'gamepad_is_connected' in t and 'gamepad_is_connected' not in o
    return o + '\n' + GAMEPAD

RULES = {
    **{(k, 'gml_Object_obj_gamecontroller_Create_0'): [ours_plus_gamepad] for k in '12345'},
    # config box grows by one row (35 px) for the "Mod Settings" entry, like 60 FPS does
    ('1', 'gml_Object_obj_darkcontroller_Draw_0'): [lambda o, b, t:
        '    draw_rectangle(xx + 60, yy + 85, xx + 580, yy + 447, false);\n'
        '    scr_darkbox(xx + 50, yy + 75, xx + 590, yy + 457);\n'],
    ('1', 'gml_Object_obj_overworldc_Draw_0'): [lambda o, b, t:
        '    var stat_right = 300;\n'
        '    var _lwc_cy = yy;\n'
        '    var _lwc_low = obj_mainchara.y > (yy + 120);\n'
        '    if (global.menuno == 9 && instance_exists(obj_dialoguer))\n'],
    # Russian title has a different letter count; scale its letter scatter like 60 FPS scales the original
    ('2', 'gml_Object_obj_gigaqueen_intro_fight_Draw_0'): [lambda o, b, t:
        '    for (i = 0; i < (sprite_get_number(sprite_index) / 2); i++)\n'
        '    {\n'
        '        xoffset[i] -= (letterspeed1 / (i + 1)) / global.fps_scale;\n'
        '    }\n'
        '    var i = (sprite_get_number(sprite_index) / 2) + 1;\n'
        '    while (i < sprite_get_number(sprite_index))\n'
        '    {\n'
        '        xoffset[i] += (letterspeed1 / (sprite_get_number(sprite_index) - i)) / global.fps_scale;\n'
        '        i++;\n'
        '    }\n'
        '    letterspeed1 -= (0.25 / global.fps_scale);\n'],
    **{(k, 'gml_Object_obj_screen_loading_Create_0'): [lambda o, b, t:
        '    alarm[0] = round(1 * global.fps_scale);\n'] for k in '24'},
    ('5', 'gml_Object_PROCESS_LOGO_Draw_0'): [lambda o, b, t:
        '        if (skiptimer >= ((30 * global.TARGET_FPS) / 15))\n'],
}

# Russian writer is a rewrite: apply the six 60 FPS edits to it directly instead of merging.
WRITER3 = [
    ('    prevent_mash_buffer--;\n', '    prevent_mash_buffer -= (1 / global.fps_scale);\n'),
    ('    if (skipme == 1)\n', '    if (skipme == 1 && (global.flag[965] == 1 || (global.time % global.fps_scale) == 0))\n'),
    ('smallface.speed = global.smspeed[fam];', 'smallface.speed = global.smspeed[fam] / global.fps_scale;'),
    ('smallface.image_speed = global.smimagespeed[fam];', 'smallface.image_speed = global.smimagespeed[fam] / global.fps_scale;'),
    ('smallface.alarm[0] = global.smalarm[fam];', 'smallface.alarm[0] = round(global.smalarm[fam] * global.fps_scale);'),
    ('    siner += 1;\n', '    siner += (1 / global.fps_scale);\n'),
]

def main():
    for (k, n), fns in RULES.items():
        p = f'{M}/M_{k}/{n}.gml'
        text = open(p, encoding='utf-8').read()
        it = iter(fns)
        text, cnt = HUNK.subn(lambda m: next(it)(m.group(1), m.group(2), m.group(3)), text)
        assert cnt == len(fns), (k, n, cnt)
        open(p, 'w', encoding='utf-8').write(text)
    r = open(f'{M}/M_3/gml_Object_obj_writer_Draw_0.R', encoding='utf-8').read()
    for a, b in WRITER3:
        a, b = a.strip(), b.strip()
        rx = re.compile(r'^([ \t]*)' + re.escape(a) + r'[ \t]*$', re.M)
        r, cnt = rx.subn(lambda m: m.group(1) + b, r)
        assert cnt == 1, (a, cnt)
    open(f'{M}/M_3/gml_Object_obj_writer_Draw_0.gml', 'w', encoding='utf-8').write(r)
    # obj_ch3_couch_video stays Russian (build.py skips it), so its conflict needs no rule
    left = [p.rsplit('/', 1)[1] for k in ('launcher', '1', '2', '3', '4', '5') for p in glob.glob(f'{M}/M_{k}/*.gml')
            if '<<<<<<<' in open(p, encoding='utf-8').read() and 'obj_ch3_couch_video_' not in p]
    if left:
        sys.exit('unresolved merge conflicts: ' + ', '.join(left))
    print('all conflicts resolved')

if __name__ == '__main__':
    main()
