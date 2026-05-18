using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace CaseChampGui.Services;

public sealed class LocalServerService : IDisposable
{
    private Process? _process;
    private bool _ownedByApp;

    public bool IsRunning => _process is { HasExited: false };

    public string? ResolvedExecutablePath { get; private set; }

    public string? ResolvedDataDirectory { get; private set; }

    public static string GetStableDataDirectory()
    {
        var explicitOverride = Environment.GetEnvironmentVariable("CASECHAMP_DATA_DIR");
        if (!string.IsNullOrWhiteSpace(explicitOverride))
        {
            return Path.GetFullPath(explicitOverride);
        }

        var projectData = TryFindProjectDataDirectory();
        if (projectData is not null)
        {
            return projectData;
        }

        string baseDir;
        if (OperatingSystem.IsWindows())
        {
            baseDir = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        }
        else if (OperatingSystem.IsMacOS())
        {
            baseDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                "Library", "Application Support");
        }
        else
        {
            var xdg = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
            baseDir = !string.IsNullOrWhiteSpace(xdg)
                ? xdg
                : Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    ".local", "share");
        }

        return Path.Combine(baseDir, "CaseChamp", "data");
    }

    public static string? TryFindProjectDataDirectory()
    {
        try
        {
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            var serverName = OperatingSystem.IsWindows() ? "dbserver.exe" : "dbserver";
            for (var depth = 0; depth < 10 && dir is not null; depth++, dir = dir.Parent)
            {
                var dataPath = Path.Combine(dir.FullName, "data");
                var hasData = Directory.Exists(dataPath);
                var hasServer = File.Exists(Path.Combine(dir.FullName, serverName))
                                || File.Exists(Path.Combine(dir.FullName, "build", serverName));
                var looksLikeRepo = File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt"))
                                    || File.Exists(Path.Combine(dir.FullName, "run.sh"));
                if (hasData && (hasServer || looksLikeRepo))
                {
                    return Path.GetFullPath(dataPath);
                }
            }
        }
        catch
        {
        }

        return null;
    }

    private static void MigrateLegacyDataIfNeeded(string targetDir)
    {
        try
        {
            Directory.CreateDirectory(targetDir);
            if (Directory.EnumerateFileSystemEntries(targetDir).GetEnumerator().MoveNext())
            {
                return;
            }

            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            for (var depth = 0; depth < 8 && dir is not null; depth++, dir = dir.Parent)
            {
                var legacy = Path.Combine(dir.FullName, "data");
                if (Directory.Exists(legacy) && !string.Equals(Path.GetFullPath(legacy), Path.GetFullPath(targetDir), StringComparison.Ordinal))
                {
                    CopyDirectory(legacy, targetDir);
                    return;
                }
            }
        }
        catch
        {
        }
    }

    private static void CopyDirectory(string src, string dst)
    {
        Directory.CreateDirectory(dst);
        foreach (var file in Directory.EnumerateFiles(src))
        {
            var name = Path.GetFileName(file);
            File.Copy(file, Path.Combine(dst, name), overwrite: true);
        }
        foreach (var sub in Directory.EnumerateDirectories(src))
        {
            var name = Path.GetFileName(sub);
            CopyDirectory(sub, Path.Combine(dst, name));
        }
    }

    public string? FindExecutable()
    {
        var candidates = BuildSearchPaths();
        foreach (var path in candidates)
        {
            try
            {
                if (File.Exists(path)) return Path.GetFullPath(path);
            }
            catch
            {
            }
        }
        return null;
    }

    public async Task<bool> StartAsync(string host, int port, Func<CancellationToken, Task<bool>> readinessCheck, CancellationToken cancellationToken = default)
    {
        if (IsRunning) return true;

        var exe = FindExecutable();
        if (exe is null)
        {
            ResolvedExecutablePath = null;
            return false;
        }

        ResolvedExecutablePath = exe;

        var dataDir = GetStableDataDirectory();
        MigrateLegacyDataIfNeeded(dataDir);
        ResolvedDataDirectory = dataDir;

        var startInfo = new ProcessStartInfo
        {
            FileName = exe,
            WorkingDirectory = Path.GetDirectoryName(Path.GetDirectoryName(exe)) ?? Path.GetDirectoryName(exe) ?? Environment.CurrentDirectory,
            CreateNoWindow = true,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        startInfo.ArgumentList.Add("--host");
        startInfo.ArgumentList.Add(host);
        startInfo.ArgumentList.Add("--port");
        startInfo.ArgumentList.Add(port.ToString());
        startInfo.ArgumentList.Add("--data-dir");
        startInfo.ArgumentList.Add(dataDir);

        try
        {
            _process = Process.Start(startInfo);
            if (_process is null) return false;
            _ownedByApp = true;

            _process.OutputDataReceived += (_, _) => { };
            _process.ErrorDataReceived += (_, _) => { };
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
        }
        catch
        {
            _process = null;
            return false;
        }

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(8);
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_process.HasExited) return false;
            try
            {
                if (await readinessCheck(cancellationToken).ConfigureAwait(false))
                {
                    return true;
                }
            }
            catch
            {
            }
            await Task.Delay(300, cancellationToken).ConfigureAwait(false);
        }
        return false;
    }

    public void Stop()
    {
        if (_process is null) return;
        if (!_ownedByApp) return;

        try
        {
            if (!_process.HasExited)
            {
                _process.Kill(entireProcessTree: true);
                _process.WaitForExit(2000);
            }
        }
        catch
        {
        }
        finally
        {
            _process.Dispose();
            _process = null;
        }
    }

    public void Dispose() => Stop();

    /// <summary>Освобождает TCP-порт (убивает процесс-слушатель). Нужно перед перезапуском устаревшего dbserver.</summary>
    public static void TryFreeTcpPort(int port)
    {
        if (port <= 0 || port > 65535) return;
        try
        {
            if (OperatingSystem.IsLinux() || OperatingSystem.IsMacOS())
            {
                using var p = Process.Start(new ProcessStartInfo
                {
                    FileName = "fuser",
                    ArgumentList = { "-k", $"{port}/tcp" },
                    CreateNoWindow = true,
                    UseShellExecute = false,
                });
                p?.WaitForExit(3000);
            }
            else if (OperatingSystem.IsWindows())
            {
                using var find = Process.Start(new ProcessStartInfo
                {
                    FileName = "cmd.exe",
                    Arguments = $"/c for /f \"tokens=5\" %a in ('netstat -ano ^| findstr :{port} ^| findstr LISTENING') do taskkill /F /PID %a",
                    CreateNoWindow = true,
                    UseShellExecute = false,
                });
                find?.WaitForExit(5000);
            }
            Thread.Sleep(400);
        }
        catch
        {
        }
    }

    private static string[] BuildSearchPaths()
    {
        var name = OperatingSystem.IsWindows() ? "dbserver.exe" : "dbserver";
        var candidates = new System.Collections.Generic.List<string>
        {
            Path.Combine(AppContext.BaseDirectory, name),
            Path.Combine(AppContext.BaseDirectory, "build", name),
            Path.Combine(Environment.CurrentDirectory, "build", name),
            Path.Combine(Environment.CurrentDirectory, name),
        };

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var depth = 0; depth < 8 && dir is not null; depth++, dir = dir.Parent)
        {
            candidates.Add(Path.Combine(dir.FullName, "build", name));
            candidates.Add(Path.Combine(dir.FullName, name));
        }

        return candidates.ToArray();
    }
}
