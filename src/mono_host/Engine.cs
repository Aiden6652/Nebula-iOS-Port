using System;
using System.IO;
using System.Reflection;
using System.Collections.Generic;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace Engine
{
    public class Engine
    {
        // 宿主（原生 dylib）调用的入口：读脚本 -> Roslyn 编译 -> Mono JIT 执行。
        public static void RunCSharp(string code)
        {
            var refs = new List<MetadataReference>();
            foreach (var loadedAsm in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (!loadedAsm.IsDynamic && !string.IsNullOrEmpty(loadedAsm.Location))
                    refs.Add(MetadataReference.CreateFromFile(loadedAsm.Location));
            }

            var tree = CSharpSyntaxTree.ParseText(code);
            var opts = new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                usings: new[] { "System" });

            var comp = CSharpCompilation.Create(
                "DynamicScript", new[] { tree }, refs, opts);

            using var ms = new MemoryStream();
            var emit = comp.Emit(ms);
            if (!emit.Success)
            {
                foreach (var d in emit.Diagnostics)
                    if (d.Severity == DiagnosticSeverity.Error)
                        Console.WriteLine("[engine] compile error: " + d);
                return;
            }

            ms.Seek(0, SeekOrigin.Begin);
            var dynamicAsm = Assembly.Load(ms.ToArray());
            var t = dynamicAsm.GetType("Script");
            var m = t?.GetMethod("Main");
            m?.Invoke(null, null);
            Console.WriteLine("[engine] C# script executed (compiled by Roslyn, JIT'd by Mono).");
        }

        // Exe 入口：直接读 script.cs.txt 跑。宿主可走 Main，也可用 RunCSharp。
        public static void Main()
        {
            string dir = Path.GetDirectoryName(typeof(Engine).Assembly.Location);
            string script = Path.Combine(dir, "script.cs.txt");
            string code = File.Exists(script) ? File.ReadAllText(script) : "/* no script */";
            Console.WriteLine("[engine] compiling + JIT-executing " + code.Length + " bytes of C# via Roslyn (Mono JIT)...");
            RunCSharp(code);
        }
    }
}
