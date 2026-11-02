// UndertaleModTool script: W/A/S/D act as the arrow keys in DELTARUNE (chapter select and every chapter).
// keyboard_set_map is runner-global, so appending it to an existing Create event of an object placed
// in the first room is enough (a new Create event could hide a parent's one); a rerun is harmless.
var gi = Data.GeneralInfo;
var room = gi.RoomOrder.Count > 0 ? gi.RoomOrder[0].Resource : Data.Rooms[0];
string name = null;
foreach (var inst in room.GameObjects) {
    var n = "gml_Object_" + inst.ObjectDefinition?.Name?.Content + "_Create_0";
    if (inst.ObjectDefinition != null && Data.Code.ByName(n) != null) { name = n; break; }
}
if (name == null) throw new ScriptException("WASD: no object with a Create event in the first room " + room.Name.Content);
Console.WriteLine("WASD: patching " + name + " (room " + room.Name.Content + ")");
var group = new UndertaleModLib.Compiler.CodeImportGroup(Data);
group.QueueAppend(name,
    "keyboard_set_map(ord(\"W\"), vk_up);\n" +
    "keyboard_set_map(ord(\"A\"), vk_left);\n" +
    "keyboard_set_map(ord(\"S\"), vk_down);\n" +
    "keyboard_set_map(ord(\"D\"), vk_right);\n");
group.Import();
