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

/* ---- 数据字典参数个数（5×Holding） ---- */
#define MASTER_PARAM_CNT        (5u)

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
/* 实例结构体：5×Holding，字段顺序与帧地址一一对应（字段顺序不可改）    */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
typedef struct {
    uint16_t th;            /* 帧0 0x0000 TH 阈值 ×100 定点 100~2000  */
    uint16_t alarm_cnt;     /* 帧1 0x0001 连续异常帧→报警  1~100      */
    uint16_t release_cnt;   /* 帧2 0x0002 连续正常帧→解除  1~100      */
    uint16_t win_len;       /* 帧3 0x0003 滑窗长 固定100 拒改        */
    uint16_t win_anom;      /* 帧4 0x0004 窗内异常帧阈值  1~100      */
} sounddog_holding_t;
#pragma pack(pop)

/* ---- 实例与字典表在 master_app.c 定义 ---- */
extern sounddog_holding_t holding_regs;
extern const mb_parameter_descriptor_t device_parameters[MASTER_PARAM_CNT];

#endif /* MB_PARAM_H */