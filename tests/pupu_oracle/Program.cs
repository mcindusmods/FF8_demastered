// PuPu oracle for tests/field_layers_test.cpp: PuPuSharp's own tile, ID and frame code (src/FF8/Sprite.cs for type 1
// fields, src/FF8/Field.cs, Entry.cs GetImageInfo), reading a .map and printing per tile its ID and per ID its frame.
// From https://github.com/julianxhokaxhiu/PuPuSharp (GPL-3.0, Omzy's PuPu with his permission); the same logic as
// Omzy's PuPu 2.1 (PuPu.exe, read from its compiled code: Sprite::.ctor, Form1::GetImageForExport).
//   dotnet build -c Release -o out          (nuget.config here: no package source needed)
//   dotnet out/PupuOracle.dll <file.map>
//   ../field_layers_test - <folder of .map files> "dotnet out/PupuOracle.dll"
using System;
using System.Collections.Generic;
using System.IO;

class Sprite
{
    public int ID; public short X, Y; public byte Layerid, Blendmode, Animid, Animstate;
    public Sprite(byte[] map, int i)
    {
        int o = i * 16;
        X = (short)BitConverter.ToUInt16(map, o);
        Y = (short)BitConverter.ToUInt16(map, o + 2);
        // type 1: o_SrcY = o + 11, o_layerid = o_SrcY + 1, blend + 1, animid + 1, animstate + 1
        Layerid = (byte)(map[o + 12] >> 1);
        Blendmode = map[o + 13];
        Animid = map[o + 14];
        Animstate = map[o + 15];
        ID = 0;
        ID += (Layerid << 24);
        ID += (Blendmode << 20);
        ID += (Animid << 12);
        ID += (Animstate << 4);
    }
}

static class Program
{
    static int Main(string[] args)
    {
        byte[] map = File.ReadAllBytes(args[0]);
        int num = (int)((map.Length / 16) - 1); // Field.cs: NumFISprites
        var sprites = new List<Sprite>();
        for (int i = 0; i < num; i++) sprites.Add(new Sprite(map, i));
        for (int i = 0; i < num; i++)
            for (int j = i + 1; j < num; j++)
                if (sprites[i].X == sprites[j].X && sprites[i].Y == sprites[j].Y)
                    if ((sprites[i].ID & 0xFFFFFFF0) == (sprites[j].ID & 0xFFFFFFF0))
                        sprites[j].ID = sprites[i].ID + 1;
        var ids = new SortedSet<uint>();
        for (int i = 0; i < num; i++) { Console.WriteLine("tile " + i + " " + ((uint)sprites[i].ID).ToString("X8")); ids.Add((uint)sprites[i].ID); }
        foreach (uint id in ids)
        {
            int Xmin = 0, Xmax = 0, Ymin = 0, Ymax = 0; // Entry.cs GetImageInfo
            for (int i = 0; i < num; i++)
                if ((sprites[i].ID & 0xFF000000) == (id & 0xFF000000))
                {
                    Xmin = Math.Min(Xmin, sprites[i].X); Xmax = Math.Max(Xmax, sprites[i].X);
                    Ymin = Math.Min(Ymin, sprites[i].Y); Ymax = Math.Max(Ymax, sprites[i].Y);
                }
            Console.WriteLine("image " + id.ToString("X8") + " " + Xmin + " " + Ymin + " " + (Xmax - Xmin + 16) + " " + (Ymax - Ymin + 16));
        }
        return 0;
    }
}
