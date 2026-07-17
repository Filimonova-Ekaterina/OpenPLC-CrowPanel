"""OPC UA transport and self-describing address-space construction."""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from asyncua import Server, ua
from asyncua.common.node import Node

from compressor_logic import StationModel
from fault_scenarios import AlarmTransition, FaultManager


LOGGER = logging.getLogger(__name__)


@dataclass
class CompressorNodes:
    """OPC UA nodes associated with one compressor model."""

    object_node: Node
    temperature: Node
    motor_current: Node
    runtime: Node
    run_status: Node
    run_command: Node
    automatic_mode: Node
    alarm_status: Node
    sensor_fault_command: Node


class OpcUaStationServer:
    """Publish a station model through OPC UA and consume HMI commands."""

    def __init__(self, equipment_configuration: dict[str, Any]) -> None:
        self.configuration = equipment_configuration
        self.server = Server()
        self.namespace_index = 0
        self.station_object: Node | None = None
        self.compressor_nodes: dict[str, CompressorNodes] = {}
        self.station_nodes: dict[str, Node] = {}
        self.event_generators: dict[str, Any] = {}

    async def initialize(self) -> None:
        """Initialize endpoint, namespace, objects, variables, and event sources."""

        server_configuration = self.configuration["opcua_server"]
        await self.server.init()
        self.server.set_endpoint(str(server_configuration["endpoint_url"]))
        self.server.set_server_name(str(server_configuration["server_name"]))
        self.namespace_index = await self.server.register_namespace(
            str(server_configuration["namespace_uri"])
        )

        await self._build_address_space()
        LOGGER.info("OPC UA address space initialized at %s", server_configuration["endpoint_url"])

    async def _build_address_space(self) -> None:
        """Build all equipment nodes from YAML names and metadata."""

        station_configuration = self.configuration["station"]
        self.station_object = await self.server.nodes.objects.add_object(
            self.namespace_index,
            str(station_configuration["browse_name"]),
        )
        await self._write_localized_attribute(
            self.station_object,
            ua.AttributeIds.DisplayName,
            str(station_configuration["display_name"]),
        )
        await self._write_localized_attribute(
            self.station_object,
            ua.AttributeIds.Description,
            str(station_configuration["description"]),
        )
        await self.station_object.set_event_notifier([ua.EventNotifier.SubscribeToEvents])

        for compressor_configuration in self.configuration["compressors"]:
            await self._add_compressor(compressor_configuration)

        receiver = await self._add_equipment_object(self.configuration["receiver"])
        self.station_nodes["receiver_pressure"] = await self._add_float_variable(
            receiver, self.configuration["receiver"]["signals"]["pressure"]
        )

        dryer = await self._add_equipment_object(self.configuration["dryer"])
        self.station_nodes["dryer_status"] = await self._add_boolean_variable(
            dryer, self.configuration["dryer"]["signals"]["status"]
        )

        network = await self._add_equipment_object(self.configuration["air_network"])
        for semantic_name in ("network_pressure", "air_demand", "total_power"):
            self.station_nodes[semantic_name] = await self._add_float_variable(
                network,
                self.configuration["air_network"]["signals"][semantic_name],
            )

        reset_configuration = station_configuration["commands"]["reset_alarms"]
        reset_node = await self._add_boolean_variable(
            self.station_object, reset_configuration, writable=True
        )
        self.station_nodes["reset_alarms"] = reset_node
        self.event_generators[str(station_configuration["id"])] = (
            await self.server.get_event_generator(
                ua.ObjectIds.BaseEventType, self.station_object
            )
        )

    async def _add_compressor(self, configuration: dict[str, Any]) -> None:
        """Add one compressor and all its configured signals and commands."""

        object_node = await self._add_equipment_object(configuration)
        signals = configuration["signals"]
        commands = configuration["commands"]
        nodes = CompressorNodes(
            object_node=object_node,
            temperature=await self._add_float_variable(object_node, signals["temperature"]),
            motor_current=await self._add_float_variable(object_node, signals["motor_current"]),
            runtime=await self._add_float_variable(object_node, signals["runtime"]),
            run_status=await self._add_boolean_variable(object_node, signals["run_status"]),
            run_command=await self._add_boolean_variable(
                object_node, commands["run_command"], writable=True
            ),
            automatic_mode=await self._add_boolean_variable(
                object_node, commands["automatic_mode"], writable=True
            ),
            alarm_status=await self._add_boolean_variable(object_node, signals["alarm_status"]),
            sensor_fault_command=await self._add_boolean_variable(
                object_node, commands["sensor_fault_command"], writable=True
            ),
        )
        identifier = str(configuration["id"])
        self.compressor_nodes[identifier] = nodes
        await object_node.set_event_notifier([ua.EventNotifier.SubscribeToEvents])
        self.event_generators[identifier] = await self.server.get_event_generator(
            ua.ObjectIds.BaseEventType, object_node
        )

    async def _add_equipment_object(self, configuration: dict[str, Any]) -> Node:
        """Add a described equipment object below the station."""

        if self.station_object is None:
            raise RuntimeError("Station object must be created first")
        node = await self.station_object.add_object(
            self.namespace_index, str(configuration["browse_name"])
        )
        await self._write_localized_attribute(
            node, ua.AttributeIds.DisplayName, str(configuration["display_name"])
        )
        await self._write_localized_attribute(
            node, ua.AttributeIds.Description, str(configuration["description"])
        )
        return node

    async def _add_float_variable(
        self, parent: Node, configuration: dict[str, Any], writable: bool = False
    ) -> Node:
        """Add an OPC UA Float variable plus discoverable engineering metadata."""

        node = await parent.add_variable(
            self.namespace_index,
            str(configuration["browse_name"]),
            ua.Variant(float(configuration["initial_value"]), ua.VariantType.Float),
        )
        await self._describe_node(node, configuration)
        if writable:
            await node.set_writable()
        return node

    async def _add_boolean_variable(
        self, parent: Node, configuration: dict[str, Any], writable: bool = False
    ) -> Node:
        """Add an OPC UA Boolean variable and optionally permit client writes."""

        node = await parent.add_variable(
            self.namespace_index,
            str(configuration["browse_name"]),
            ua.Variant(bool(configuration["initial_value"]), ua.VariantType.Boolean),
        )
        await self._describe_node(node, configuration)
        if writable:
            await node.set_writable()
        return node

    async def _describe_node(self, node: Node, configuration: dict[str, Any]) -> None:
        """Write human-readable attributes and optional engineering properties."""

        await self._write_localized_attribute(
            node, ua.AttributeIds.DisplayName, str(configuration["display_name"])
        )
        await self._write_localized_attribute(
            node, ua.AttributeIds.Description, str(configuration["description"])
        )
        if "engineering_unit" in configuration:
            await node.add_property(
                self.namespace_index,
                "EngineeringUnit",
                str(configuration["engineering_unit"]),
            )
        if "semantic_role" in configuration:
            await node.add_property(
                self.namespace_index,
                "SemanticRole",
                str(configuration["semantic_role"]),
            )
        if "minimum" in configuration:
            await node.add_property(
                self.namespace_index,
                "Minimum",
                ua.Variant(float(configuration["minimum"]), ua.VariantType.Float),
            )
        if "maximum" in configuration:
            await node.add_property(
                self.namespace_index,
                "Maximum",
                ua.Variant(float(configuration["maximum"]), ua.VariantType.Float),
            )

    async def _write_localized_attribute(
        self, node: Node, attribute_id: ua.AttributeIds, text: str
    ) -> None:
        """Write a LocalizedText node attribute using the asyncua core API."""

        await node.write_attribute(
            attribute_id,
            ua.DataValue(
                ua.Variant(ua.LocalizedText(text), ua.VariantType.LocalizedText)
            ),
        )

    async def read_commands(self, station: StationModel) -> bool:
        """Read client-writable command variables and apply them to the model."""

        for compressor in station.compressors:
            nodes = self.compressor_nodes[compressor.identifier]
            compressor.automatic_mode = bool(await nodes.automatic_mode.read_value())
            compressor.manual_run_command = bool(await nodes.run_command.read_value())
            compressor.sensor_fault_command = bool(
                await nodes.sensor_fault_command.read_value()
            )
        return bool(await self.station_nodes["reset_alarms"].read_value())

    async def publish(self, station: StationModel, faults: FaultManager) -> None:
        """Publish one consistent model snapshot to OPC UA variables."""

        for compressor in station.compressors:
            nodes = self.compressor_nodes[compressor.identifier]
            await nodes.temperature.write_value(
                ua.Variant(faults.displayed_temperature(compressor), ua.VariantType.Float)
            )
            await nodes.motor_current.write_value(
                ua.Variant(faults.displayed_current(compressor), ua.VariantType.Float)
            )
            await nodes.runtime.write_value(
                ua.Variant(compressor.state.runtime_hours, ua.VariantType.Float)
            )
            await nodes.run_status.write_value(
                ua.Variant(compressor.state.is_running, ua.VariantType.Boolean)
            )
            if compressor.automatic_mode:
                await nodes.run_command.write_value(
                    ua.Variant(compressor.state.is_running, ua.VariantType.Boolean)
                )
            await nodes.alarm_status.write_value(
                ua.Variant(compressor.state.alarm_active, ua.VariantType.Boolean)
            )

        await self.station_nodes["receiver_pressure"].write_value(
            ua.Variant(station.receiver_pressure_bar, ua.VariantType.Float)
        )
        await self.station_nodes["dryer_status"].write_value(
            ua.Variant(station.dryer_running, ua.VariantType.Boolean)
        )
        await self.station_nodes["network_pressure"].write_value(
            ua.Variant(station.network_pressure_bar, ua.VariantType.Float)
        )
        await self.station_nodes["air_demand"].write_value(
            ua.Variant(station.air_demand_m3h, ua.VariantType.Float)
        )
        await self.station_nodes["total_power"].write_value(
            ua.Variant(station.total_power_kw, ua.VariantType.Float)
        )

    async def acknowledge_reset_command(self) -> None:
        """Return the momentary alarm-reset command to False after processing."""

        await self.station_nodes["reset_alarms"].write_value(
            ua.Variant(False, ua.VariantType.Boolean)
        )
        for nodes in self.compressor_nodes.values():
            await nodes.sensor_fault_command.write_value(
                ua.Variant(False, ua.VariantType.Boolean)
            )

    async def emit_alarm_event(self, transition: AlarmTransition) -> None:
        """Emit a standard OPC UA BaseEventType for an alarm state transition."""

        generator = self.event_generators.get(transition.source_identifier)
        if generator is None:
            generator = self.event_generators[str(self.configuration["station"]["id"])]
        state_text = "ACTIVE" if transition.active else "CLEARED"
        generator.event.Message = ua.LocalizedText(
            f"[{transition.alarm_code}] {state_text}: {transition.message}"
        )
        generator.event.Severity = transition.severity
        generator.event.Time = datetime.now(timezone.utc)
        await generator.trigger()
        LOGGER.warning(
            "Alarm %s for %s: %s",
            state_text,
            transition.source_name,
            transition.message,
        )
