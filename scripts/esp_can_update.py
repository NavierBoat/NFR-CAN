import requests
import sys
import hashlib
import time
import math
from os.path import basename

Import("env")

try:
    import can
    import cantools
    from tqdm.auto import tqdm
except ImportError:
    env.Execute("$PYTHONEXE -m pip install can")
    env.Execute("$PYTHONEXE -m pip install cantools")
    env.Execute("$PYTHONEXE -m pip install tqdm")
    env.Execute("$PYTHONEXE -m pip install progressbar")
    import can
    import cantools
    from tqdm.auto import tqdm

try:
    import configparser
except ImportError:
    import ConfigParser as configparser
# project_config = configparser.ConfigParser()
# project_config.read("platformio.ini")
# can_update_config = {k: v for k, v in project_config.items("can_update")}
can_update_config = env.GetProjectConfig().items("can_update", as_dict=True)
db = cantools.database.load_file("esp_can_update.dbc")


def on_upload(source, target, env):
    firmware_path = str(source[0])

    with open(firmware_path, "rb") as firmware:
        firmware_bytes = firmware.read()
        md5 = hashlib.md5(firmware_bytes).digest()
        print(hashlib.md5(firmware_bytes).hexdigest())
        firmware.seek(0)

        data_message = db.get_message_by_name("update_data_message")
        data_message.frame_id = int(can_update_config.get("update_message_id"), 0)
        info_message = db.get_message_by_name("update_info_message")
        info_message.frame_id = data_message.frame_id + 1
        progress_message = db.get_message_by_name("update_progress_message")
        progress_message.frame_id = data_message.frame_id + 2

        can_bus = can.interface.Bus(
            "can0",
            bustype="socketcan",
            bitrate=int(can_update_config.get("update_baud")),
        )
        if can_bus is None:
            env.Execute(
                "sudo ip link set up can0 type can bitrate "
                + can_update_config.get("update_baud")
            )
            can_bus = can.interface.Bus(
                "can0",
                bustype="socketcan",
                bitrate=int(can_update_config.get("update_baud")),
            )

        try:
            info_message_data = info_message.encode(
                {
                    "message_type": 1,
                    "update_md5": md5[i * 4]
                    + (md5[(i * 4) + 1] << 8)
                    + (md5[(i * 4) + 2] << 16)
                    + (md5[(i * 4) + 3] << 24),
                    "update_md5_idx": i,
                }
            )
            can_bus.send(
                can.Message(
                    arbitration_id=info_message.frame_id, data=info_message_data
                )
            )
        except:
            env.Execute(
                "sudo ip link set up can0 type can bitrate "
                + can_update_config.get("update_baud")
            )
            can_bus = can.interface.Bus(
                "can0",
                bustype="socketcan",
                bitrate=int(can_update_config.get("update_baud")),
            )

        received_md5 = False
        while not received_md5:
            for i in range(4):
                info_message_data = info_message.encode(
                    {
                        "message_type": 1,
                        "update_md5": md5[i * 4]
                        + (md5[(i * 4) + 1] << 8)
                        + (md5[(i * 4) + 2] << 16)
                        + (md5[(i * 4) + 3] << 24),
                        "update_md5_idx": i,
                    }
                )
                can_bus.send(
                    can.Message(
                        arbitration_id=info_message.frame_id, data=info_message_data
                    )
                )
                time.sleep(0.02)
            msg = can_bus.recv(0.1)
            if (msg != None) and msg.arbitration_id == progress_message.frame_id:
                received_progress_msg = db.decode_message(
                    "update_progress_message", msg.data
                )
                if received_progress_msg["received_md5"]:
                    received_md5 = True
            while msg is not None:
                msg = can_bus.recv(0.00001)

        info_message_data = info_message.encode(
            {"message_type": 0, "update_length": len(firmware_bytes)}
        )
        received_len = False

        while not received_len:
            can_bus.send(
                can.Message(
                    arbitration_id=info_message.frame_id, data=info_message_data
                )
            )
            time.sleep(0.01)
            msg = can_bus.recv(0.05)
            if (msg != None) and msg.arbitration_id == progress_message.frame_id:
                received_progress_msg = db.decode_message(
                    "update_progress_message", msg.data
                )
                if received_progress_msg["received_len"]:
                    received_len = True
            while msg is not None:
                msg = can_bus.recv(0.00001)
        tqdm._instances.clear()
        bar = tqdm(
            desc="Upload Progress",
            total=len(firmware_bytes),
            ncols=80,
            unit="B",
            unit_scale=True,
            position=0,
            leave=True,
        )

        msgs_in_block = 2

        max_block_written = -1
        last_update = 0
        while max_block_written < math.floor((len(firmware_bytes) + 6) / 7) - 1:
            for j in range(msgs_in_block):
                data_idx = max_block_written + j + 1
                if data_idx < math.floor((len(firmware_bytes) + 6) / 7):
                    data = 0
                    for b in range(min(7, len(firmware_bytes) - (data_idx * 7))):
                        data = data + (firmware_bytes[(data_idx * 7) + b] << (8 * b))
                    data_message_data = data_message.encode(
                        {
                            "data_block_index_low": data_idx & 0xFF,
                            "update_data": data,
                        }
                    )
                    can_bus.send(
                        can.Message(
                            arbitration_id=data_message.frame_id
                            + (
                                (data_idx << 3) & 0x1FFFF800
                            ),  # 18 bits of CAN extended ID, not including standard ID
                            data=data_message_data,
                        )
                    )
                    time.sleep(1 / 3817)

                msg = can_bus.recv(1 / 3817)
                while msg is not None:
                    if msg.arbitration_id == progress_message.frame_id:
                        received_progress_msg = db.decode_message(
                            "update_progress_message", msg.data
                        )
                        # print(db.decode_message("update_progress_message", data_message_data))
                        # print(i)
                        if received_progress_msg["written"] == True:
                            if (
                                received_progress_msg["update_block_idx"]
                                > max_block_written
                            ):
                                max_block_written = received_progress_msg[
                                    "update_block_idx"
                                ]
                        else:
                            if (
                                received_progress_msg["update_block_idx"] - 1
                                > max_block_written
                            ):
                                max_block_written = (
                                    received_progress_msg["update_block_idx"] - 1
                                )
                    msg = can_bus.recv(0.000001)

            if max_block_written - last_update >= 1024:
                bar.update((max_block_written - last_update) * 7)
                last_update = max_block_written
                # time.sleep(0.1)
        bar.close()
        can_bus.shutdown()


try:
    if env.GetProjectOption("upload_can") == "y":
        DefaultEnvironment().Replace(UPLOADCMD=on_upload)
except:
    pass
