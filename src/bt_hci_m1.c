/*
 *  hci.c  --  OS9-Bluetooth  |  M1 HCI command/event layer (device-agnostic).
 *
 *  Bring-up sequence, each step advanced by the previous command's completion:
 *    Reset -> Read_Local_Version -> Read_BD_ADDR -> Read_Buffer_Size
 *          -> Set_Event_Mask -> Write_Scan_Enable -> Inquiry -> (results)
 *
 *  Read_Local_Version / Read_BD_ADDR identify the controller (and expose the
 *  shared-MAC signature of counterfeit CSR8510 dongles). Standard HCI, so the
 *  same code drives an external dongle or a Mini's internal CSR module.
 *
 *  Opcodes/event codes adapted from prior art:
 *    vendor/bt-control-center/USB Bluetooth/HCI/{HCI_Constants.h,HCI_Types.h}
 */
#include "bt_hci_m1.h"

/* opcode groups + packing (Bluetooth Core HCI) */
#define OGF_LINK_CTL          0x01
#define OGF_HOST_CTL          0x03
#define OGF_INFO_PARAM        0x04
#define HCI_OPCODE(ogf,ocf)   ((UInt16)(((ocf) & 0x03ff) | ((ogf) << 10)))

#define OPC_RESET             HCI_OPCODE(OGF_HOST_CTL,   0x0003)  /* 0x0C03 */
#define OPC_SET_EVENT_MASK    HCI_OPCODE(OGF_HOST_CTL,   0x0001)  /* 0x0C01 */
#define OPC_WRITE_SCAN_ENABLE HCI_OPCODE(OGF_HOST_CTL,   0x001A)  /* 0x0C1A */
#define OPC_READ_LOCAL_VER    HCI_OPCODE(OGF_INFO_PARAM, 0x0001)  /* 0x1001 */
#define OPC_READ_BUFFER_SIZE  HCI_OPCODE(OGF_INFO_PARAM, 0x0005)  /* 0x1005 */
#define OPC_READ_BD_ADDR      HCI_OPCODE(OGF_INFO_PARAM, 0x0009)  /* 0x1009 */
#define OPC_INQUIRY           HCI_OPCODE(OGF_LINK_CTL,   0x0001)  /* 0x0401 */

/* event codes */
#define EVT_INQUIRY_COMPLETE  0x01
#define EVT_INQUIRY_RESULT    0x02
#define EVT_CMD_COMPLETE      0x0E
#define EVT_CMD_STATUS        0x0F

static UInt8 gCmd[32];   /* command scratch -- one command outstanding at a time */

static UInt16 rd16le(const UInt8 *p) { return (UInt16)(p[0] | (p[1] << 8)); }

/* Build [opcode-lo, opcode-hi, plen, params...] (USB wire order) and send. */
static OSStatus SendCmd(UInt16 opcode, const UInt8 *params, UInt8 plen)
{
    UInt8 i;
    gCmd[0] = (UInt8)(opcode & 0xFF);
    gCmd[1] = (UInt8)((opcode >> 8) & 0xFF);
    gCmd[2] = plen;
    for (i = 0; i < plen; i++) gCmd[3 + i] = params[i];
    return BT_SendHCICommand(gCmd, (UInt32)(3 + plen));
}

/* On each command's completion, issue the next one in the sequence. */
static void AdvanceInit(UInt16 completedOpcode)
{
    switch (completedOpcode)
    {
        case OPC_RESET:            SendCmd(OPC_READ_LOCAL_VER,   nil, 0); break;
        case OPC_READ_LOCAL_VER:   SendCmd(OPC_READ_BD_ADDR,     nil, 0); break;
        case OPC_READ_BD_ADDR:     SendCmd(OPC_READ_BUFFER_SIZE, nil, 0); break;

        case OPC_READ_BUFFER_SIZE: {
            /* Bluetooth 1.1 default event mask 0x00001FFFFFFFFFFF, LSB first. */
            static const UInt8 mask[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0x1F,0x00,0x00 };
            SendCmd(OPC_SET_EVENT_MASK, mask, 8);
            break;
        }
        case OPC_SET_EVENT_MASK: {
            static const UInt8 scan[1] = { 0x02 };   /* page scan enabled */
            SendCmd(OPC_WRITE_SCAN_ENABLE, scan, 1);
            break;
        }
        case OPC_WRITE_SCAN_ENABLE: {
            /* GIAC 0x9E8B33 (LSB first), ~10s, unlimited responses. */
            static const UInt8 inq[5] = { 0x33,0x8B,0x9E, 0x08, 0x00 };
            SendCmd(OPC_INQUIRY, inq, 5);
            break;
        }
        /* Inquiry replies via CMD_STATUS + INQUIRY_RESULT(s) + INQUIRY_COMPLETE. */
    }
}

void HCI_Start(void)
{
    BT_Log("\pHCI: M1 sequence start (reset, identify, configure, inquiry)", 0);
    SendCmd(OPC_RESET, nil, 0);
}

void HCI_HandleEvent(const UInt8 *evt, UInt32 len)
{
    UInt8 code;
    if (len < 2) return;
    code = evt[0];

    switch (code)
    {
        case EVT_CMD_COMPLETE: {
            UInt16       opcode;
            UInt8        status;
            const UInt8 *ret;                 /* return parameters */
            if (len < 6) break;
            opcode = rd16le(&evt[3]);
            status = evt[5];
            ret    = &evt[6];

            switch (opcode) {
                case OPC_RESET:
                    BT_Log("\pHCI: Reset complete (status in value)", status);
                    BT_Record(kRecResetStatus, status);
                    break;
                case OPC_READ_LOCAL_VER:
                    BT_Log("\pHCI: local HCI_Version (in value)", ret[0]);
                    BT_Log("\pHCI: Manufacturer_ID (in value)", rd16le(&ret[4]));
                    BT_Record(kRecHciVersion,  ret[0]);
                    BT_Record(kRecManufacturer, rd16le(&ret[4]));
                    break;
                case OPC_READ_BD_ADDR:   /* LE at ret[0..5]; shared value => clone */
                    BT_Log("\pHCI: BD_ADDR high 3 bytes (in value)",
                           ((UInt32)ret[5]<<16)|((UInt32)ret[4]<<8)|ret[3]);
                    BT_Log("\pHCI: BD_ADDR low 3 bytes (in value)",
                           ((UInt32)ret[2]<<16)|((UInt32)ret[1]<<8)|ret[0]);
                    BT_Record(kRecBdAddrHi,
                              ((UInt32)ret[5]<<16)|((UInt32)ret[4]<<8)|ret[3]);
                    BT_Record(kRecBdAddrLo,
                              ((UInt32)ret[2]<<16)|((UInt32)ret[1]<<8)|ret[0]);
                    break;
                case OPC_READ_BUFFER_SIZE:
                    BT_Log("\pHCI: ACL data packet length (in value)", rd16le(&ret[0]));
                    BT_Log("\pHCI: total ACL packets (in value)", rd16le(&ret[3]));
                    BT_Record(kRecAclLen,   rd16le(&ret[0]));
                    BT_Record(kRecAclCount, rd16le(&ret[3]));
                    break;
                case OPC_SET_EVENT_MASK:
                    BT_Log("\pHCI: SetEventMask complete (status in value)", status);
                    BT_Record(kRecSetMaskStatus, status);
                    break;
                case OPC_WRITE_SCAN_ENABLE:
                    BT_Log("\pHCI: WriteScanEnable complete (status in value)", status);
                    BT_Record(kRecScanEnStatus, status);
                    break;
            }
            AdvanceInit(opcode);
            break;
        }

        case EVT_CMD_STATUS:
            BT_Log("\pHCI: Command Status - Inquiry started (opcode in value)", rd16le(&evt[4]));
            BT_Record(kRecCmdStatusOpcode, rd16le(&evt[4]));
            break;

        case EVT_INQUIRY_RESULT: {
            UInt8        n;
            const UInt8 *a;
            if (len < 3) break;
            n = evt[2];
            a = &evt[3];                       /* first response BD_ADDR (LE) */
            BT_Log("\pHCI: Inquiry Result - responses (in value)", n);
            BT_Record(kRecInqResponses, n);
            if (n >= 1 && len >= 9) {
                BT_Log("\pHCI:  device BD_ADDR high (in value)",
                       ((UInt32)a[5]<<16)|((UInt32)a[4]<<8)|a[3]);
                BT_Log("\pHCI:  device BD_ADDR low (in value)",
                       ((UInt32)a[2]<<16)|((UInt32)a[1]<<8)|a[0]);
                BT_Record(kRecInqDevHi, ((UInt32)a[5]<<16)|((UInt32)a[4]<<8)|a[3]);
                BT_Record(kRecInqDevLo, ((UInt32)a[2]<<16)|((UInt32)a[1]<<8)|a[0]);
            }
            break;
        }

        case EVT_INQUIRY_COMPLETE:
            BT_Log("\p*** M1 COMPLETE: controller commanded, events parsed, inquiry done ***", 0);
            /* 0x100 | status, so a zero slot means NOT RECEIVED rather than
             * "received, status 0". Those were indistinguishable up to v1.1 and
             * one run could not be read because of it. */
            BT_Record(kRecInqComplete, 0x100UL | ((len >= 3) ? evt[2] : 0xFF));
            break;

        default:
            BT_Log("\pHCI: event received (event code in value)", code);
            BT_Record(kRecLastOtherEvent, code);
            break;
    }
}
