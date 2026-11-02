// Read-only: prints every code entry backed up by the translation (NAME_old) and the code count.
Console.WriteLine("COUNT " + Data.Code.Count);
foreach (var c in Data.Code) { var n = c.Name.Content; if (n.EndsWith("_old")) Console.WriteLine("OLD " + n.Substring(0, n.Length - 4)); }
