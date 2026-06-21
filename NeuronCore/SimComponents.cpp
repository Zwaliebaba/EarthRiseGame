// SimComponents.cpp — single definition site for the sim ECS component type IDs
// (Components.h). Exactly one TU per executable must instantiate these; ERServer,
// ERHeadless and the UWP client get them by compiling this file (the test runner
// uses Tests_Sim.h instead).

#include "pch.h"
#include "Components.h"

NEURON_DEFINE_COMPONENT(Neuron::Sim::Transform, Neuron::Sim::Slot_Transform);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Velocity,  Neuron::Sim::Slot_Velocity);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BaseTag,   Neuron::Sim::Slot_BaseTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShipTag,   Neuron::Sim::Slot_ShipTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NetId,     Neuron::Sim::Slot_NetId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Health,    Neuron::Sim::Slot_Health);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ShapeId,   Neuron::Sim::Slot_ShapeId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Fuel,      Neuron::Sim::Slot_Fuel);
NEURON_DEFINE_COMPONENT(Neuron::Sim::NavState,  Neuron::Sim::Slot_NavState);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BeaconTag, Neuron::Sim::Slot_BeaconTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::OwnerId,         Neuron::Sim::Slot_OwnerId);
NEURON_DEFINE_COMPONENT(Neuron::Sim::ResourceNodeTag, Neuron::Sim::Slot_ResourceNodeTag);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Cargo,           Neuron::Sim::Slot_Cargo);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Storage,         Neuron::Sim::Slot_Storage);
NEURON_DEFINE_COMPONENT(Neuron::Sim::BuildQueue,      Neuron::Sim::Slot_BuildQueue);
NEURON_DEFINE_COMPONENT(Neuron::Sim::FleetMember,     Neuron::Sim::Slot_FleetMember);
NEURON_DEFINE_COMPONENT(Neuron::Sim::Sensor,          Neuron::Sim::Slot_Sensor);
