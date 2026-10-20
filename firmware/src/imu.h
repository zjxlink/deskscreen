#pragma once

// 板载加速度计（BOX-3 的 ICM-42607-P）。没有的板子上 imu_begin() 返回 false

// 要在 audio_begin() 之后调用（I2C 已经初始化）
bool imu_begin();

// 读一次加速度，单位 g
bool imu_read(float &x, float &y, float &z);
