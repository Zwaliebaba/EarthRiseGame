#pragma once
// ShapeCatalog.h - canonical registry of every renderable mesh asset shipped
// under EarthRise/Assets/Shapes (DirectXTK .cmo + <stem>1/2/3.dds diffuse/normal/
// spec). GENERATED from the asset tree; edit the generator, not this file by hand.
//
// The catalog is the single source of truth shared by the server (assigns a
// stable ShapeId per spawned entity) and the client (loads the mesh + diffuse by
// ShapeId). Pure data + constexpr lookups - no platform dependencies, so it lives
// in NeuronCore and is usable from console tools, the server, and the UWP client.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Components.h" // EntityKind

namespace Neuron::Sim
{
  // Broad asset family. Drives default EntityKind, colour, and display scale.
  enum class ShapeCategory : uint8_t
  {
    Asteroid = 0,
    Crate = 1,
    Decoration = 2,
    Hull = 3,
    Jumpgate = 4,
    SpaceObject = 5,
    SpecialObject = 6,
    Station = 7,
  };

  // A single mesh asset. 'cmoPath' is the package-relative path (backslashes,
  // ready for the UWP installed-location join); the diffuse texture is the same
  // path with the ".cmo" suffix replaced by "1.dds" (normal "2.dds", specular
  // "3.dds") — the client derives it when loading.
  struct ShapeDef
  {
    uint16_t         id;
    ShapeCategory    category;
    std::string_view name;
    std::string_view cmoPath;
  };

  inline constexpr ShapeDef SHAPES[] = {
    { 0, ShapeCategory::Asteroid, "Asteroid01Rock", "Assets\\Shapes\\Asteroids\\Asteroid01Rock.cmo" },
    { 1, ShapeCategory::Asteroid, "Asteroid02Rock", "Assets\\Shapes\\Asteroids\\Asteroid02Rock.cmo" },
    { 2, ShapeCategory::Asteroid, "Asteroid03Rock", "Assets\\Shapes\\Asteroids\\Asteroid03Rock.cmo" },
    { 3, ShapeCategory::Asteroid, "Asteroid04Ice", "Assets\\Shapes\\Asteroids\\Asteroid04Ice.cmo" },
    { 4, ShapeCategory::Asteroid, "Asteroid05Ice", "Assets\\Shapes\\Asteroids\\Asteroid05Ice.cmo" },
    { 5, ShapeCategory::Asteroid, "Asteroid06Lava", "Assets\\Shapes\\Asteroids\\Asteroid06Lava.cmo" },
    { 6, ShapeCategory::Crate, "Crate01", "Assets\\Shapes\\Crates\\Crate01.cmo" },
    { 7, ShapeCategory::Crate, "Crate02", "Assets\\Shapes\\Crates\\Crate02.cmo" },
    { 8, ShapeCategory::Crate, "CrateAlien01", "Assets\\Shapes\\Crates\\CrateAlien01.cmo" },
    { 9, ShapeCategory::Decoration, "BrokenAsteroid01", "Assets\\Shapes\\Decorations\\BrokenAsteroid01.cmo" },
    { 10, ShapeCategory::Decoration, "Satellite01", "Assets\\Shapes\\Decorations\\Satellite01.cmo" },
    { 11, ShapeCategory::Decoration, "Satellite01Rust", "Assets\\Shapes\\Decorations\\Satellite01Rust.cmo" },
    { 12, ShapeCategory::Decoration, "StationWreck01", "Assets\\Shapes\\Decorations\\StationWreck01.cmo" },
    { 13, ShapeCategory::Hull, "HullAvalancheMk2", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullAvalancheMk2.cmo" },
    { 14, ShapeCategory::Hull, "HullBansheeMk2", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullBansheeMk2.cmo" },
    { 15, ShapeCategory::Hull, "HullBoomerang", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullBoomerang.cmo" },
    { 16, ShapeCategory::Hull, "HullFang", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullFang.cmo" },
    { 17, ShapeCategory::Hull, "HullFreighter", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullFreighter.cmo" },
    { 18, ShapeCategory::Hull, "HullScarabMk2", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullScarabMk2.cmo" },
    { 19, ShapeCategory::Hull, "HullStingray", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullStingray.cmo" },
    { 20, ShapeCategory::Hull, "HullStryker", "Assets\\Shapes\\Hulls\\HeavyFrigates\\HullStryker.cmo" },
    { 21, ShapeCategory::Hull, "HullAurora", "Assets\\Shapes\\Hulls\\HeavyCruisers\\HullAurora.cmo" },
    { 22, ShapeCategory::Hull, "HullDeathBringer", "Assets\\Shapes\\Hulls\\HeavyCruisers\\HullDeathBringer.cmo" },
    { 23, ShapeCategory::Hull, "HullHammer", "Assets\\Shapes\\Hulls\\HeavyCruisers\\HullHammer.cmo" },
    { 24, ShapeCategory::Hull, "HullWasp", "Assets\\Shapes\\Hulls\\HeavyCruisers\\HullWasp.cmo" },
    { 25, ShapeCategory::Hull, "HullYamato", "Assets\\Shapes\\Hulls\\HeavyCruisers\\HullYamato.cmo" },
    { 26, ShapeCategory::Hull, "HullAvalanche", "Assets\\Shapes\\Hulls\\LightFrigates\\HullAvalanche.cmo" },
    { 27, ShapeCategory::Hull, "HullBanshee", "Assets\\Shapes\\Hulls\\LightFrigates\\HullBanshee.cmo" },
    { 28, ShapeCategory::Hull, "HullEndeavor", "Assets\\Shapes\\Hulls\\LightFrigates\\HullEndeavor.cmo" },
    { 29, ShapeCategory::Hull, "HullHeretic", "Assets\\Shapes\\Hulls\\LightFrigates\\HullHeretic.cmo" },
    { 30, ShapeCategory::Hull, "HullScarab", "Assets\\Shapes\\Hulls\\LightFrigates\\HullScarab.cmo" },
    { 31, ShapeCategory::Hull, "HullShuttle", "Assets\\Shapes\\Hulls\\LightFrigates\\HullShuttle.cmo" },
    { 32, ShapeCategory::Hull, "HullAsteria", "Assets\\Shapes\\Hulls\\LightCruisers\\HullAsteria.cmo" },
    { 33, ShapeCategory::Hull, "HullCrab", "Assets\\Shapes\\Hulls\\LightCruisers\\HullCrab.cmo" },
    { 34, ShapeCategory::Hull, "HullCrabOrder", "Assets\\Shapes\\Hulls\\LightCruisers\\HullCrabOrder.cmo" },
    { 35, ShapeCategory::Hull, "HullOrca", "Assets\\Shapes\\Hulls\\LightCruisers\\HullOrca.cmo" },
    { 36, ShapeCategory::Hull, "HullOrcaFreedom", "Assets\\Shapes\\Hulls\\LightCruisers\\HullOrcaFreedom.cmo" },
    { 37, ShapeCategory::Hull, "HullStingrayMk2", "Assets\\Shapes\\Hulls\\LightCruisers\\HullStingrayMk2.cmo" },
    { 38, ShapeCategory::Hull, "HullStingrayMk2Fanatics", "Assets\\Shapes\\Hulls\\LightCruisers\\HullStingrayMk2Fanatics.cmo" },
    { 39, ShapeCategory::Hull, "HullFreighterNpc", "Assets\\Shapes\\Hulls\\Other\\HullFreighterNpc.cmo" },
    { 40, ShapeCategory::Hull, "HullHive", "Assets\\Shapes\\Hulls\\Other\\HullHive.cmo" },
    { 41, ShapeCategory::Hull, "HullMegalisk", "Assets\\Shapes\\Hulls\\Other\\HullMegalisk.cmo" },
    { 42, ShapeCategory::Hull, "HullPestBrown", "Assets\\Shapes\\Hulls\\Other\\HullPestBrown.cmo" },
    { 43, ShapeCategory::Hull, "HullPestRed", "Assets\\Shapes\\Hulls\\Other\\HullPestRed.cmo" },
    { 44, ShapeCategory::Hull, "HullPiratePlatform", "Assets\\Shapes\\Hulls\\Other\\HullPiratePlatform.cmo" },
    { 45, ShapeCategory::Hull, "HullTurretPlatform", "Assets\\Shapes\\Hulls\\Other\\HullTurretPlatform.cmo" },
    { 46, ShapeCategory::Hull, "HullTurretPlatformMk2", "Assets\\Shapes\\Hulls\\Other\\HullTurretPlatformMk2.cmo" },
    { 47, ShapeCategory::Hull, "HullTurretPlatformMk3", "Assets\\Shapes\\Hulls\\Other\\HullTurretPlatformMk3.cmo" },
    { 48, ShapeCategory::Hull, "HullTurretXengatarn", "Assets\\Shapes\\Hulls\\Other\\HullTurretXengatarn.cmo" },
    { 49, ShapeCategory::Hull, "HullViolator", "Assets\\Shapes\\Hulls\\Other\\HullViolator.cmo" },
    { 50, ShapeCategory::Jumpgate, "Jumpgate01", "Assets\\Shapes\\Jumpgates\\Jumpgate01.cmo" },
    { 51, ShapeCategory::SpaceObject, "DebrisGenericBarrel01", "Assets\\Shapes\\SpaceObjects\\DebrisGenericBarrel01.cmo" },
    { 52, ShapeCategory::SpaceObject, "DebrisGenericBarrel02", "Assets\\Shapes\\SpaceObjects\\DebrisGenericBarrel02.cmo" },
    { 53, ShapeCategory::SpaceObject, "DebrisGenericBarrel03", "Assets\\Shapes\\SpaceObjects\\DebrisGenericBarrel03.cmo" },
    { 54, ShapeCategory::SpaceObject, "DebrisGenericBeam01", "Assets\\Shapes\\SpaceObjects\\DebrisGenericBeam01.cmo" },
    { 55, ShapeCategory::SpaceObject, "DebrisGenericPipe01", "Assets\\Shapes\\SpaceObjects\\DebrisGenericPipe01.cmo" },
    { 56, ShapeCategory::SpaceObject, "DebrisGenericSolarpanel01", "Assets\\Shapes\\SpaceObjects\\DebrisGenericSolarpanel01.cmo" },
    { 57, ShapeCategory::SpaceObject, "DebrisGenericTransitor01", "Assets\\Shapes\\SpaceObjects\\DebrisGenericTransitor01.cmo" },
    { 58, ShapeCategory::SpaceObject, "DebrisGenericWreck01", "Assets\\Shapes\\SpaceObjects\\DebrisGenericWreck01.cmo" },
    { 59, ShapeCategory::SpaceObject, "Mine01", "Assets\\Shapes\\SpaceObjects\\Mine01.cmo" },
    { 60, ShapeCategory::SpecialObject, "ErgrekTerminal", "Assets\\Shapes\\SpecialObjects\\ErgrekTerminal.cmo" },
    { 61, ShapeCategory::SpecialObject, "PlatformRefuel", "Assets\\Shapes\\SpecialObjects\\PlatformRefuel.cmo" },
    { 62, ShapeCategory::SpecialObject, "PlatformRepair", "Assets\\Shapes\\SpecialObjects\\PlatformRepair.cmo" },
    { 63, ShapeCategory::SpecialObject, "ScurvyRemains", "Assets\\Shapes\\SpecialObjects\\ScurvyRemains.cmo" },
    { 64, ShapeCategory::SpecialObject, "XengatarnDevice01", "Assets\\Shapes\\SpecialObjects\\XengatarnDevice01.cmo" },
    { 65, ShapeCategory::Station, "Business01", "Assets\\Shapes\\Stations\\Business01.cmo" },
    { 66, ShapeCategory::Station, "Military01", "Assets\\Shapes\\Stations\\Military01.cmo" },
    { 67, ShapeCategory::Station, "Mining01", "Assets\\Shapes\\Stations\\Mining01.cmo" },
    { 68, ShapeCategory::Station, "Outpost01", "Assets\\Shapes\\Stations\\Outpost01.cmo" },
    { 69, ShapeCategory::Station, "Science01", "Assets\\Shapes\\Stations\\Science01.cmo" },
  };

  inline constexpr uint16_t SHAPE_COUNT = 70;
  static_assert(sizeof(SHAPES) / sizeof(SHAPES[0]) == SHAPE_COUNT, "shape count mismatch");

  inline constexpr uint16_t INVALID_SHAPE_ID = 0xFFFF;

  // Default entity kind for a category (the client uses it for colour/scale
  // fallback; the server stamps it into the snapshot when no explicit kind set).
  inline constexpr EntityKind KindForCategory(ShapeCategory c) noexcept
  {
    switch (c)
    {
      case ShapeCategory::Asteroid: return EntityKind::Asteroid;
      case ShapeCategory::Crate: return EntityKind::LootContainer;
      case ShapeCategory::Decoration: return EntityKind::Decoration;
      case ShapeCategory::Hull: return EntityKind::Ship;
      case ShapeCategory::Jumpgate: return EntityKind::Structure;
      case ShapeCategory::SpaceObject: return EntityKind::Debris;
      case ShapeCategory::SpecialObject: return EntityKind::Structure;
      case ShapeCategory::Station: return EntityKind::Station;
    }
    return EntityKind::Unknown;
  }

  // Look up a shape by id; returns nullptr if out of range.
  inline constexpr const ShapeDef* ShapeById(uint16_t id) noexcept
  {
    return (id < SHAPE_COUNT) ? &SHAPES[id] : nullptr;
  }

  // Find a shape id by (case-sensitive) name; returns INVALID_SHAPE_ID if absent.
  inline constexpr uint16_t ShapeIdByName(std::string_view name) noexcept
  {
    for (const auto& s : SHAPES)
      if (s.name == name) return s.id;
    return INVALID_SHAPE_ID;
  }

  // First shape id in a category (INVALID_SHAPE_ID if the category is empty).
  inline constexpr uint16_t FirstShapeOfCategory(ShapeCategory c) noexcept
  {
    for (const auto& s : SHAPES)
      if (s.category == c) return s.id;
    return INVALID_SHAPE_ID;
  }
} // namespace Neuron::Sim
