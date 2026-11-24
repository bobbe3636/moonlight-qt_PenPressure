"""Adds two small hooks to moonlight-common-c (the protocol library, an upstream submodule that
this fork doesn't fork): control messages from the host with a type in 0x7C00-0x7CFF (the vwacom
extension: the host's pointer) go to a new listener callback, hostMessage, and LiSendHostMessage()
sends one of that range to the host.

The build runs this on the submodule before compiling (see .github/workflows/build-windows.yml):
    python scripts/patch-common-c.py moonlight-common-c/moonlight-common-c
Every anchor must match exactly once, so an upstream change in these places stops the build
instead of building something half patched. Running it twice changes nothing.
"""
import os
import sys

MARK = "vwacom extension"


def patch(root, rel, edits):
    path = os.path.join(root, rel)
    with open(path, "rb") as f:
        data = f.read()
    crlf = b"\r\n" in data
    text = data.decode("utf-8").replace("\r\n", "\n")
    if MARK in text:
        print(f"{rel}: already patched")
        return
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise SystemExit(f"{rel}: expected one match, found {count}: {old[:80]!r}")
        text = text.replace(old, new)
    if crlf:
        text = text.replace("\n", "\r\n")
    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))
    print(f"{rel}: patched")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-common-c.py <moonlight-common-c directory>")
    root = sys.argv[1]

    patch(root, "src/Limelight.h", [
        (
            "typedef void(*ConnListenerSetControllerLED)(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);\n",
            "typedef void(*ConnListenerSetControllerLED)(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);\n"
            "\n"
            "// vwacom extension: a control message from the host with a type in 0x7C00-0x7CFF (the host's\n"
            "// pointer, for clients that draw it themselves). Called on the control stream's receive thread:\n"
            "// copy what you need and return quickly.\n"
            "typedef void(*ConnListenerHostMessage)(uint16_t type, const uint8_t* data, uint32_t length);\n",
        ),
        (
            "    ConnListenerSetAdaptiveTriggers setAdaptiveTriggers;\n"
            "} CONNECTION_LISTENER_CALLBACKS, *PCONNECTION_LISTENER_CALLBACKS;\n",
            "    ConnListenerSetAdaptiveTriggers setAdaptiveTriggers;\n"
            "    ConnListenerHostMessage hostMessage; // vwacom extension\n"
            "} CONNECTION_LISTENER_CALLBACKS, *PCONNECTION_LISTENER_CALLBACKS;\n",
        ),
        (
            "int LiSendUtf8TextEvent(const char *text, unsigned int length);\n",
            "int LiSendUtf8TextEvent(const char *text, unsigned int length);\n"
            "\n"
            "// vwacom extension: a control message of type 0x7C00-0x7CFF to the host (reliable).\n"
            "// Returns 0 on success.\n"
            "int LiSendHostMessage(uint16_t type, const void* data, uint32_t length);\n",
        ),
    ])

    patch(root, "src/FakeCallbacks.c", [
        (
            "static void fakeClSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {}\n",
            "static void fakeClSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {}\n"
            "static void fakeClHostMessage(uint16_t type, const uint8_t* data, uint32_t length) {} // vwacom extension\n",
        ),
        (
            "    .setAdaptiveTriggers = fakeClSetAdaptiveTriggers,\n",
            "    .setAdaptiveTriggers = fakeClSetAdaptiveTriggers,\n"
            "    .hostMessage = fakeClHostMessage,\n",
        ),
        (
            "        if ((*clCallbacks)->setAdaptiveTriggers == NULL) {\n"
            "            (*clCallbacks)->setAdaptiveTriggers = fakeClSetAdaptiveTriggers;\n"
            "        }\n",
            "        if ((*clCallbacks)->setAdaptiveTriggers == NULL) {\n"
            "            (*clCallbacks)->setAdaptiveTriggers = fakeClSetAdaptiveTriggers;\n"
            "        }\n"
            "        if ((*clCallbacks)->hostMessage == NULL) {\n"
            "            (*clCallbacks)->hostMessage = fakeClHostMessage;\n"
            "        }\n",
        ),
    ])

    patch(root, "src/ControlStream.c", [
        (
            "            // Process client callbacks in a separate thread\n"
            "            if (needsAsyncCallback(ctlHdr->type)) {\n",
            "            // vwacom extension: the host's messages of this range go to the client as they are\n"
            "            if ((ctlHdr->type & 0xFF00) == 0x7C00) {\n"
            "                if (packetLength >= (int)sizeof(*ctlHdr)) {\n"
            "                    ListenerCallbacks.hostMessage(ctlHdr->type, (const uint8_t*)(ctlHdr + 1),\n"
            "                                                  (uint32_t)(packetLength - (int)sizeof(*ctlHdr)));\n"
            "                }\n"
            "                free(ctlHdr);\n"
            "                continue;\n"
            "            }\n"
            "\n"
            "            // Process client callbacks in a separate thread\n"
            "            if (needsAsyncCallback(ctlHdr->type)) {\n",
        ),
        (
            "// Called by the input stream to flush queued packets before a batching wait\n",
            "// vwacom extension: a control message of type 0x7C00-0x7CFF to the host\n"
            "int LiSendHostMessage(uint16_t type, const void* data, uint32_t length) {\n"
            "    if ((type & 0xFF00) != 0x7C00 || peer == NULL || stopping || AppVersionQuad[0] < 5) {\n"
            "        return -1;\n"
            "    }\n"
            "\n"
            "    return sendMessageAndForget((short)type, length, data, CTRL_CHANNEL_GENERIC,\n"
            "                                ENET_PACKET_FLAG_RELIABLE, false) ? 0 : -1;\n"
            "}\n"
            "\n"
            "// Called by the input stream to flush queued packets before a batching wait\n",
        ),
    ])


if __name__ == "__main__":
    main()
