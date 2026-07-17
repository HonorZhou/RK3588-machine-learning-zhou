---
title: ROS2 Gazebo 仿真环境学习笔记
author: QClaw
date: 2026-07-14
type: 技术
tags: [ROS2, Gazebo, 仿真, AWS, Warehouse, House, Nav2, SLAM, slam_toolbox, RTAB-Map, Sim2Real, 四足机器人, SCAN-Planner, 局部规划]
source: conversation
---

# ROS2 Gazebo 仿真环境学习笔记

> 持续更新的 Gazebo 仿真学习笔记，涵盖环境搭建、世界模型、导航 SLAM 验证等。

## 目录

- [[#1. Gazebo 简介|1. Gazebo 简介]]
- [[#2. Gazebo vs Isaac Gym|2. Gazebo vs Isaac Gym]]
- [[#3. AWS Small Warehouse / Small House|3. AWS Small Warehouse / Small House]]
- [[#4. 环境搭建|4. 环境搭建]]
- [[#5. 典型工作流|5. 典型工作流]]
- [[#6. 与四足机器狗训练方向的关系|6. 与四足机器狗训练方向的关系]]
- [[#7. Nav2 导航框架详解|7. Nav2 导航框架详解]]
- [[#8. SCAN-Planner 与 Nav2 的关系|8. SCAN-Planner 与 Nav2 的关系]]
- [[#9. 完整技术栈架构图|9. 完整技术栈架构图]]
- [[#10. 相关笔记|10. 相关笔记]]

---

## 1. Gazebo 简介

Gazebo 是 ROS/ROS2 生态中最主流的**物理仿真环境**，相当于"机器人的虚拟世界"。

### 基本架构

```
机器人模型（URDF/SDF）
    ↓
Gazebo 物理引擎（重力、碰撞、摩擦、惯性）
    ↓
虚拟传感器（相机、LiDAR、IMU、深度相机）
    ↓
ROS2 话题发布（和真机器人完全一样的接口）
```

### 核心能力

- 模拟发布 `/scan`（LiDAR）、`/odom`（里程计）、`/camera/image`（相机）等话题
- 控制命令发给虚拟机器人，行为和真机器人一致
- **Sim2Real 核心**：代码不改，仿真里跑通了，换到真机器人上也能跑

---

## 2. Gazebo vs Isaac Gym

| | Gazebo | Isaac Gym/Lab |
|---|---|---|
| **定位** | 导航/感知仿真 | RL 训练加速器 |
| **物理引擎** | ODE/Bullet/DART | PhysX（GPU 加速） |
| **强项** | 传感器模拟、ROS 集成 | 万级并行训练 |
| **速度** | 慢（接近实时） | 快（数千倍加速） |
| **用途** | 测试导航/SLAM/避障 | 训练步态/控制策略 |
| **图形** | 中等 | 高（可选） |

**简单说**：Isaac Gym 训练怎么走，Gazebo 测试怎么导航，两者互补。

---

## 3. AWS Small Warehouse / Small House

AWS（亚马逊云）开源的一组 **Gazebo 仿真世界模型**，提供现成的室内场景。

| 世界 | 场景内容 | 用途 |
|------|----------|------|
| **AWS Small Warehouse** | 小型仓库，有货架、箱子、通道 | 测试机器人在仓储环境中的导航、避障 |
| **AWS Small House** | 小型住宅，有房间、门、家具 | 测试机器人在家庭环境中的导航 |

本质是 3D 模型文件（meshes + 配置），加载到 Gazebo 里就是可视化的室内环境，机器人放进去就能跑 SLAM、导航、建图。

---

## 4. 环境搭建

### 4.1 安装

```bash
# ROS2 Humble / Iron（推荐）
sudo apt install ros-humble-aws-robomaker-small-warehouse-world
sudo apt install ros-humble-aws-robomaker-small-house-world

# 或者从 GitHub 编译
cd ~/ros2_ws/src
git clone https://github.com/aws-robotics/aws-robomaker-small-warehouse-world
git clone https://github.com/aws-robotics/aws-robomaker-small-house-world
cd ~/ros2_ws && colcon build
```

### 4.2 启动世界

```bash
# AWS Small Warehouse
ros2 launch aws_robomaker_small_warehouse_world small_warehouse.launch.py

# AWS Small House
ros2 launch aws_robomaker_small_house_world small_house.launch.py
```

启动后会打开 Gazebo 窗口，看到 3D 室内场景。

### 4.3 放入机器人 + 导航

```bash
# 终端1：Gazebo + 世界 + 机器人模型
ros2 launch your_robot_pkg gazebo_warehouse.launch.py

# 终端2：Nav2 导航
ros2 launch nav2_bringup navigation_launch.py

# 终端3：slam_toolbox 建图
ros2 launch slam_toolbox online_async_launch.py

# 终端4：RViz 可视化 + 发导航目标
ros2 run rviz2 rviz2
```

---

## 5. 典型工作流

```
1. Gazebo 加载 AWS Warehouse 世界
2. 机器人模型（如 Go1 URDF）spawn 到世界中
3. Gazebo 模拟发布 /scan、/odom、/camera
4. slam_toolbox 接收 /scan 建图，发布 /map
5. Nav2 接收 /map + /scan，规划路径，发布 /cmd_vel
6. Gazebo 接收 /cmd_vel，驱动机器人移动
7. RViz 可视化地图 + 机器人位置 + 导航路径
```

### 数据流图

```
┌─────────┐    /scan     ┌──────────────┐    /map    ┌──────────┐
│  Gazebo │ ──────────→  │ slam_toolbox │ ────────→  │   Nav2   │
│ (仿真)  │    /odom     │   (建图)     │            │  (导航)  │
│         │ ──────────→  └──────────────┘            └────┬─────┘
│         │                                                  │
│         │  ←───────────────── /cmd_vel ──────────────────┘
└─────────┘

                    ┌──────────┐
                    │  RViz2   │  ← 可视化 /map /scan /path
                    └──────────┘
```

---

## 6. 与四足机器狗训练方向的关系

```
训练阶段：
  Isaac Gym/Lab + RSL-RL + PPO → 训练步态策略（走路、跑步）

部署阶段（Sim2Real）：
  真实环境需要自主导航 → SLAM + Nav2
  ↓
  先在 Gazebo + AWS Warehouse 里测试导航
  ↓
  再迁移到真实 Go1 上

对应论文（arxiv 2505.02272）：
  RTAB-Map + slam_toolbox + ROS2 Nav
  → 先在 Gazebo 里验证
  → 再到真机上跑
```

### 完整 Sim2Real 路线

| 阶段 | 工具 | 目标 |
|------|------|------|
| 步态训练 | Isaac Gym + RSL-RL + PPO | 学会走路 |
| 导航验证 | Gazebo + AWS Warehouse | 测试 SLAM + Nav2 |
| 真机部署 | Go1 + RGB-D + IMU | Sim2Real 迁移 |

---

## 7. Nav2 导航框架详解

### Nav2 是什么

**Nav2**（Navigation2）是 ROS2 官方的导航框架，负责让机器人**从 A 点自主移动到 B 点**，途中自动避障。

简单说：你告诉它"去那个角落"，它自己规划路线、躲开障碍、把机器人开过去。

### 核心工作流程

```
你：给一个目标坐标（x, y）
  ↓
Nav2：
  1. 查地图，全局规划路线（A* / Dijkstra）
  2. 实时检测障碍，局部调整路径（DWB / MPPI）
  3. 计算速度命令，发布 /cmd_vel
  4. 机器人移动 → 到达目标
```

### 核心功能

| 功能 | 说明 |
|------|------|
| **全局路径规划** | 看整张地图，算一条从起点到终点的路线 |
| **局部避障** | 走的过程中遇到新障碍，实时绕开 |
| **代价地图（Costmap）** | 把地图分成网格，标记哪里能走哪里不能走 |
| **行为树（Behavior Tree）** | 控制导航逻辑：卡住了后退、重试、换路线 |
| **恢复行为** | 比如原地旋转清障碍、后退脱困 |
| **多种运动模式** | 前进、原地转向、平滑弧线等 |

### Nav2 架构

```
Nav2 导航框架（整体）
├── 全局规划器（Global Planner）→ A*/Dijkstra，算粗路线
├── 局部规划器（Local Planner） → DWB/MPPI（默认），可替换为 SCAN-Planner
├── 代价地图（Costmap）         → 2D 网格，标记障碍/自由区域
├── 行为树（Behavior Tree）     → 控制导航决策逻辑
└── 恢复行为（Recovery）        → 原地旋转、后退脱困
```

### 在四足机器狗项目中的位置

```
步态控制（Isaac Gym 训练）    → 让狗能走路
    ↓
Nav2 导航（ROS2）             → 让狗知道往哪走、怎么绕障碍
    ↓
SLAM 建图（slam_toolbox）     → 给 Nav2 提供地图
    ↓
传感器（RGB-D + IMU）         → 给 SLAM 和 Nav2 提供感知
```

- **没有 Nav2**：狗能走，但你得手动遥控，或者只能走预设路线
- **有 Nav2**：你发一个目标点，狗自己看地图、规划路线、躲障碍走过去

---

## 8. SCAN-Planner 与 Nav2 的关系

### SCAN-Planner 是什么

**SCAN-Planner**（论文 arXiv:2606.19555v1）是专门为四足机器人设计的**局部路径规划器**，解决"给定粗略全局路线后，机器人如何实时绕障"的问题。

详见 [[SCAN-Planner四足机器人自主绕障原理详解]]。

### Nav2 默认 vs SCAN-Planner

Nav2 默认的局部规划器是 DWB / MPPI，把机器人当圆看待。**SCAN-Planner 可以看作四足专用的增强版局部规划器**：

| | Nav2 默认（DWB/MPPI） | SCAN-Planner |
|---|---|---|
| **机器人模型** | 单个圆 | 双圆柱（朝向感知） |
| **地图维度** | 2D Costmap | 3D 占据地图 |
| **避障方式** | 2D 平面绕 | 贴地 3D 绕障，不随意上下飞 |
| **长距离** | 依赖全局地图 | 滑动局部地图 + 边界回退 |
| **适用平台** | 轮式/差速 | 四足机器人 |

### SCAN-Planner 六大核心原理

1. **双圆柱身体模型** — 两个竖直圆柱近似长条形身体，碰撞检测跟随朝向变化
2. **B 样条轨迹** — 三次均匀 B 样条表示轨迹，优化控制点实现位置/速度/加速度平滑
3. **碰撞反弹优化** — 借鉴 EGO-Planner，碰撞后构造局部无碰撞引导路径，不维护完整 ESDF
4. **Projected A* 贴地搜索** — 水平网格搜索 + 地面跟随插值，不像无人机自由 3D 飞行
5. **滑动局部地图** — 以机器人为中心的固定大小窗口，离开的释放，新进入的等待更新
6. **死胡同恢复** — 边界回退策略，目标不可达时在边界加虚拟自由层引导走出困境

### 替换关系

```
Nav2 框架
├── 全局规划器 → 保留（A*/Dijkstra）
├── 局部规划器 → 替换为 SCAN-Planner ←
├── Costmap    → 升级为 3D 占据地图
└── 其他组件   → 保留
```

---

## 9. 完整技术栈架构图

```
感知层：
  FAST-LIO2（LiDAR-IMU里程计）→ 定位 + 点云
  RGB-D + IMU（低成本方案）→ 论文 2505.02272 的方案
        ↓
建图层：
  slam_toolbox（2D SLAM）
  ROG-Map / 滑动局部地图（3D 占据）
        ↓
规划层：
  Nav2 全局规划器 → 粗路线
  SCAN-Planner → 局部绕障轨迹（B样条优化）
        ↓
控制层：
  Isaac Gym + RSL-RL + PPO 训练的步态策略
        ↓
仿真验证：
  Gazebo + AWS Warehouse → Nav2 + SLAM 测试
  Isaac Gym → 步态训练
        ↓
真机部署：
  Unitree Go1/Go2 + Jetson Orin NX
```

### 四足机器狗学习路线总览

| 层次 | 技术 | 对应笔记 |
|------|------|----------|
| 步态训练 | Isaac Gym + RSL-RL + PPO | 宇树Go1训练方法笔记 |
| 导航框架 | Nav2 + Gazebo + AWS Warehouse | 本笔记 |
| 局部规划 | SCAN-Planner（双圆柱+B样条+贴地A*） | [[SCAN-Planner四足机器人自主绕障原理详解]] |
| SLAM建图 | RTAB-Map + slam_toolbox + scan稳定化 | [[Wander熵-低成本四足机器人定位建图导航-20260714]] |
| 硬件平台 | RK3588 / Go1 + RGB-D + IMU | [[RK3588-嵌入式Linux学习路径与机器狗部署答疑-20260710]] |
| Bootloader | U-Boot 启动流程/编译/适配 | [[U-Boot学习笔记]] |

---

## 10. 相关笔记

- [[U-Boot学习笔记]]
- [[Wander熵-低成本四足机器人定位建图导航-20260714]]
- [[RK3588-嵌入式Linux学习路径与机器狗部署答疑-20260710]]
- [[RK3588-仿真模拟学习方案-20260710]]

---

## 更新日志

- 2026-07-14：创建笔记，涵盖 Gazebo 简介、AWS Warehouse/House 世界模型、环境搭建、典型工作流、与四足机器狗训练方向的关系
- 2026-07-14：补充 Nav2 导航框架详解、SCAN-Planner 与 Nav2 的关系、完整技术栈架构图、四足机器狗学习路线总览
