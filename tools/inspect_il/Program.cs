using Mono.Cecil;
using System;
using System.Linq;

// 用 Mono.Cecil 检视 DLL 的类型/方法 IL，定位 resolver 注入点。
// 用法：
//   inspect_il <dll>                     列出所有类型全名
//   inspect_il <dll> <TypeFilter>        列出匹配类型的所有方法 IL（TypeFilter 为空串则全部）
//   inspect_il <dll> <TypeFilter> <Meth> 只 dump 指定方法
class P
{
    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("usage: inspect_il <dll> [TypeFilter] [Method]"); return 1; }
        string path = args[0];
        string tf = args.Length > 1 ? args[1] : null;
        string mf = args.Length > 2 ? args[2] : null;
        if (!System.IO.File.Exists(path)) { Console.Error.WriteLine("找不到 " + path); return 1; }

        ModuleDefinition mod;
        try { mod = ModuleDefinition.ReadModule(path, new ReaderParameters { InMemory = true, ReadWrite = false }); }
        catch (Exception e) { Console.Error.WriteLine("读失败: " + e.Message); return 1; }

        Console.WriteLine($"# {path}  Assembly={mod.Assembly?.Name?.FullName}  Types={mod.Types.Count}");

        var types = mod.Types
            .Where(t => tf == null || tf == "" || t.FullName.Contains(tf))
            .ToList();

        if (tf == null)
        {
            // 仅列类型名
            foreach (var t in mod.Types.OrderBy(t => t.FullName))
                Console.WriteLine("  T: " + t.FullName);
            return 0;
        }

        foreach (var t in types)
        {
            Console.WriteLine($"\n=== TYPE: {t.FullName} (methods={t.Methods.Count}) ===");
            foreach (var m in t.Methods)
            {
                if (mf != null && !m.Name.Contains(mf)) continue;
                DumpMethod(m);
            }
        }
        return 0;
    }

    static void DumpMethod(MethodDefinition m)
    {
        Console.WriteLine($"  --- {m.FullName}");
        if (!m.HasBody) { Console.WriteLine("      (no body / extern)"); return; }
        foreach (var ins in m.Body.Instructions)
        {
            string op = ins.Operand == null ? "" : Short(ins.Operand.ToString());
            Console.WriteLine($"      {ins.Offset,6}  {ins.OpCode.Name,-16} {op}");
        }
    }

    static string Short(string s)
    {
        if (s.Length <= 90) return s;
        int cut = s.IndexOf(',');
        if (cut > 0 && cut < 90) return s.Substring(0, cut) + " …";
        return s.Substring(0, 90) + "…";
    }
}
