/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mb_param.h — SoundDog Modbus 主站数据字典（Holding 寄存器，0 基址）
 *
 * 结构/宏仿照官方 mb_serial_master 例程 modbus_params.h 的
 * `结构体 + offsetof>>1` 定位方式（C-07：只换字典表，不断官方模式）。
 *
 * R34/R35 0 基址：本结构体字段均为 uint16_t（2B），
 * 每字段 offsetof>>1 即帧地址号：TH=0x0000 … WIN_ANOM=0x0004。
 * 禁止 +1；主站发帧地址字段 = mb_reg_start 原样（预研 §1.4/GW-01）。
 *
 * 范围宏来源：SoundDog firmware/soundDog/BSP/param.h（唯一事实源，禁止自造）。
 */
#ifndef MB_PARAM_H
#define MB_PARAM_H

#include <stdint.h>
#include <stddef.h>     /* offsetof */
#include "mbcontroller.h"
#include "esp_modbus_master.h"   /* mb_parameter_descriptor_t / PARAM_TYPE_U16 显式来源 */

/* ---- 从站地址（SoundDog 0x01）绝对常量 ---- */
#define SD_DEVICE_ADDR          (0x01u)

/* ---- 从站保持寄存器个数（9×Holding，0..4 可写参数 + 5..8 只读遥测） ---- */
#define MASTER_PARAM_CNT        (9u)

/* ---- FEAT-ESPGW-04：数据字典【CID 个数】= 5 ----
 *   CID 0 = 整块读（9 个寄存器一次读完，01 03 00 00 00 09）
 *   CID 1~4 = 单寄存器写（FEAT-ESPGW-05 下行控制）
 *
 * 读用整块、写用单条，是刻意的非对称设计：
 *  - 读：RTU 响应帧无事务号/起始地址 ⇒ 逐寄存器读会「值整体前移一格」（官方 #16）
 *        ⇒ 必须一次读整块（一个响应 / 一次 CRC）。
 *  - 写：从站 eMBRegHoldingCB 的整批写是【原子】的（任一越界 ⇒ 全批拒、保留旧值），
 *        且 WIN_LEN 固定拒写 ⇒ ⛔ 不能整块写 5 个，否则必定全批失败。
 * ⇒ 写必须逐字段、各用一个 CID。
 * MASTER_PARAM_CNT 为「寄存器个数(9)」，打印按 CID#n 逐字段输出。
 * ⚠️ 寄存器数扩展【不增加 CID】：5..8 是只读遥测，无写 CID，
 *    所以 MASTER_CID_CNT 仍为 5（1 个整块读 + 4 个单寄存器写）。 */
#define MASTER_CID_CNT          (5u)

/* ---- 写用 CID（与寄存器号对应；WIN_LEN 无写 CID，属刻意缺失） ---- */
#define CID_READ_ALL            (0u)
#define CID_WR_TH               (1u)   /* 寄存器 0 */
#define CID_WR_ALARM_CNT        (2u)   /* 寄存器 1 */
#define CID_WR_RELEASE_CNT      (3u)   /* 寄存器 2 */
#define CID_WR_WIN_ANOM         (4u)   /* 寄存器 4 */
#define CID_WR_NONE             (0xFFFFu)  /* 该寄存器不可写（寄存器 3 = WIN_LEN） */

/* ---- 从站范围宏（来自 SoundDog param.h，禁止自造） ---- */
#define SD_PARAM_TH_MIN_x100    100u    /* 1.0 */
#define SD_PARAM_TH_MAX_x100    2000u   /* 20.0 */
#define SD_PARAM_FRAME_MIN      1u
#define SD_PARAM_FRAME_MAX      100u
#define SD_PARAM_WIN_LEN_FIX    100u    /* 滑窗固定=100（拒改） */

/* ---- 参数 offset 宏（官方同款） ---- */
#define HOLD_OFFSET(field)      ((uint16_t)(offsetof(sounddog_holding_t, field) + 1))
#define HOLD_REG_START(field)   (HOLD_OFFSET(field) >> 1)
#define HOLD_REG_SIZE(field)    (sizeof(((sounddog_holding_t*)0)->field) >> 1)

/* 参数选项（min,max,step） */
#define OPTS(min_val, max_val, step_val) \
    { .opt1 = (min_val), .opt2 = (max_val), .opt3 = (step_val) }

#define STR(s) ((const char *)(s))

/* ------------------------------------------------------------------ */
/* 实例结构体：9×Holding，字段顺序与帧地址一一对应（字段顺序不可改）    */
/*   0..4 = 可写参数（FEAT-04 整块读 / FEAT-05 逐条写）                */
/*   5..8 = 只读遥测（从站 MODBUS_REG_HOLDING_NUM 9u，写操作被 MB_ENOREG 拒）*/
/*   ⚠️ HOLD_REG_START 的 (offsetof+1)>>1 技巧对 2 字节字段正好给出     */
/*      0..8 的连续地址，新增字段必须仍为 uint16_t 且保持顺序。         */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
typedef struct {
    uint16_t th;            /* 帧0 0x0000 TH 阈值 ×100 定点 100~2000  */
    uint16_t alarm_cnt;     /* 帧1 0x0001 连续异常帧→报警  1~100      */
    uint16_t release_cnt;   /* 帧2 0x0002 连续正常帧→解除  1~100      */
    uint16_t win_len;       /* 帧3 0x0003 滑窗长 固定100 拒改        */
    uint16_t win_anom;      /* 帧4 0x0004 窗内异常帧阈值  1~100      */
    uint16_t temp;          /* 帧5 0x0005 温度 ×10 + 500 偏移（只读） */
                            /*          753 = 25.3℃；-45℃ → 50       */
    uint16_t humi;          /* 帧6 0x0006 湿度 ×10（只读）612=61.2%RH */
    uint16_t vib_rms;       /* 帧7 0x0007 振动 RMS (mg)（只读）       */
                            /*          MPU6050 未接入前恒 0          */
    uint16_t vib_peak;      /* 帧8 0x0008 振动峰值 (mg)（只读）同上   */
} sounddog_holding_t;
#pragma pack(pop)

/* ---- 整块读的字节数：9×u16 = 18（mbc_master_set_param_data 以 param_size 为准） ---- */
#define SD_HOLD_ALL_BYTES       (sizeof(sounddog_holding_t))

/* ---- 遥测值换算（协议层的偏移/缩放只在这一处定义，勿在别处重复） ---- */
#define SD_TEMP_OFFSET          (500)   /* 从站：reg5 = g_temp_x10 + 500 */
#define SD_TEMP_SCALE           (10.0)  /* ×10 定点 */
#define SD_HUMI_SCALE           (10.0)

/* ---- 实例与字典表在 master_app.c 定义 ---- */
extern sounddog_holding_t holding_regs;
/* FEAT-ESPGW-04：字典表长度 = MASTER_CID_CNT（1），不再等于寄存器个数 */
extern const mb_parameter_descriptor_t device_parameters[MASTER_CID_CNT];

#endif /* MB_PARAM_H */