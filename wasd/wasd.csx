// UndertaleModTool script: W/A/S/D act as the arrow keys.
// obj_time is created once at game start and is persistent, so one keyboard_set_map call per key is enough.
var group = new UndertaleModLib.Compiler.CodeImportGroup(Data);
group.QueueAppend("gml_Object_obj_time_Create_0",
    "keyboard_set_map(ord(\"W\"), vk_up);\n" +
    "keyboard_set_map(ord(\"A\"), vk_left);\n" +
    "keyboard_set_map(ord(\"S\"), vk_down);\n" +
    "keyboard_set_map(ord(\"D\"), vk_right);\n");
group.Import();
