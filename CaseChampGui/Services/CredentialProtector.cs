using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace CaseChampGui.Services;

public static class CredentialProtector
{
    private const string AppSalt = "CaseChampGui.Account.v1";

    public static string Encrypt(string plain)
    {
        var key = DeriveKey();
        var iv = RandomNumberGenerator.GetBytes(12);
        var plainBytes = Encoding.UTF8.GetBytes(plain);
        var cipher = new byte[plainBytes.Length];
        var tag = new byte[16];
        using (var aes = new AesGcm(key, 16))
        {
            aes.Encrypt(iv, plainBytes, cipher, tag);
        }
        using var ms = new MemoryStream();
        ms.Write(iv);
        ms.Write(cipher);
        ms.Write(tag);
        return Convert.ToBase64String(ms.ToArray());
    }

    public static string? TryDecrypt(string? payload)
    {
        if (string.IsNullOrEmpty(payload)) return null;
        try
        {
            var raw = Convert.FromBase64String(payload);
            if (raw.Length < 12 + 16) return null;
            var iv = raw.AsSpan(0, 12);
            var cipherLen = raw.Length - 12 - 16;
            var cipher = raw.AsSpan(12, cipherLen);
            var tag = raw.AsSpan(12 + cipherLen, 16);
            var plain = new byte[cipherLen];
            var key = DeriveKey();
            using (var aes = new AesGcm(key, 16))
            {
                aes.Decrypt(iv, cipher, tag, plain);
            }
            return Encoding.UTF8.GetString(plain);
        }
        catch
        {
            return null;
        }
    }

    private static byte[] DeriveKey()
    {
        var material = Encoding.UTF8.GetBytes(
            $"{Environment.UserName}@{Environment.MachineName}|{AppSalt}");
        return SHA256.HashData(material);
    }
}
