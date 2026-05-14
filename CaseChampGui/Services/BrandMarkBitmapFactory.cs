using System;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace CaseChampGui.Services;

public enum SidebarBrandVariant
{
    Dark,
    Light,
    Iu5,
}

public static class BrandMarkBitmapFactory
{
    public static Bitmap CreateSidebarBrand(SidebarBrandVariant variant)
    {
        var uri = variant switch
        {
            SidebarBrandVariant.Dark => new Uri("avares://CaseChampGui/Assets/brand_sidebar_dark.png"),
            SidebarBrandVariant.Light => new Uri("avares://CaseChampGui/Assets/brand_sidebar_light.png"),
            SidebarBrandVariant.Iu5 => new Uri("avares://CaseChampGui/Assets/brand_sidebar_iu5.png"),
            _ => throw new ArgumentOutOfRangeException(nameof(variant)),
        };

        using var stream = AssetLoader.Open(uri);
        return new Bitmap(stream);
    }
}
