"""Integration checks for the live asyncua server and client contract."""

from __future__ import annotations

import asyncio
from pathlib import Path
import unittest

from asyncua import Client, ua
import yaml

from compressor_logic import StationModel
from fault_scenarios import AlarmTransition, FaultManager
from opcua_server import OpcUaStationServer


class OpcUaIntegrationTests(unittest.IsolatedAsyncioTestCase):
    """Verify data types, access rights, commands, and event generation."""

    async def asyncSetUp(self) -> None:
        configuration_directory = Path(__file__).parent / "configuration"
        with (configuration_directory / "equipment.yaml").open(
            "r", encoding="utf-8"
        ) as configuration_file:
            self.equipment_configuration = yaml.safe_load(configuration_file)
        with (configuration_directory / "alarms.yaml").open(
            "r", encoding="utf-8"
        ) as configuration_file:
            self.alarm_configuration = yaml.safe_load(configuration_file)["alarms"]
        self.endpoint_url = "opc.tcp://127.0.0.1:4841/compressed-air-station-test/"
        self.equipment_configuration["opcua_server"]["endpoint_url"] = self.endpoint_url
        self.station = StationModel(self.equipment_configuration)
        self.fault_manager = FaultManager(self.alarm_configuration)
        self.opcua_server = OpcUaStationServer(self.equipment_configuration)
        await self.opcua_server.initialize()
        await self.opcua_server.server.start()

    async def asyncTearDown(self) -> None:
        await self.opcua_server.server.stop()

    async def test_address_space_commands_and_alarm_event(self) -> None:
        compressor = self.station.compressors[0]
        server_nodes = self.opcua_server.compressor_nodes[compressor.identifier]

        async with Client(url=self.endpoint_url) as client:
            temperature_node = client.get_node(server_nodes.temperature.nodeid)
            run_command_node = client.get_node(server_nodes.run_command.nodeid)
            automatic_mode_node = client.get_node(server_nodes.automatic_mode.nodeid)
            run_status_node = client.get_node(server_nodes.run_status.nodeid)

            self.assertEqual(
                ua.VariantType.Float,
                await temperature_node.read_data_type_as_variant_type(),
            )
            self.assertEqual(
                ua.VariantType.Boolean,
                await run_command_node.read_data_type_as_variant_type(),
            )
            temperature_access = await temperature_node.read_attribute(
                ua.AttributeIds.UserAccessLevel
            )
            command_access = await run_command_node.read_attribute(
                ua.AttributeIds.UserAccessLevel
            )
            self.assertFalse(
                temperature_access.Value.Value
                & (1 << int(ua.AccessLevel.CurrentWrite))
            )
            self.assertTrue(
                command_access.Value.Value
                & (1 << int(ua.AccessLevel.CurrentWrite))
            )

            await automatic_mode_node.write_value(False)
            await run_command_node.write_value(False)
            await self.opcua_server.read_commands(self.station)
            self.station.update(0.1)
            self.fault_manager.evaluate(self.station, 0.1)
            await self.opcua_server.publish(self.station, self.fault_manager)
            self.assertFalse(await run_status_node.read_value())

            await run_command_node.write_value(True)
            await self.opcua_server.read_commands(self.station)
            self.station.update(0.1)
            await self.opcua_server.publish(self.station, self.fault_manager)
            self.assertTrue(await run_status_node.read_value())

            transition = AlarmTransition(
                source_identifier=compressor.identifier,
                source_name=compressor.display_name,
                alarm_code="INTEGRATION_TEST",
                message="Event generation check",
                severity=500,
                active=True,
            )
            await self.opcua_server.emit_alarm_event(transition)


if __name__ == "__main__":
    unittest.main()
