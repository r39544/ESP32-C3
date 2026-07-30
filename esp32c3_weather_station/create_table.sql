-- =============================================
-- ESP32-C3 气象站 - 数据表
-- 数据库: WeatherDB (需要先创建)
-- =============================================

-- 创建数据库 (如果还没有)
IF NOT EXISTS (SELECT name FROM sys.databases WHERE name = 'WeatherDB')
    CREATE DATABASE WeatherDB;
GO

USE WeatherDB;
GO

-- 创建数据表
IF NOT EXISTS (SELECT * FROM sys.tables WHERE name = 'WeatherReadings')
BEGIN
    CREATE TABLE dbo.WeatherReadings (
        Id          INT IDENTITY(1,1)   PRIMARY KEY,   -- 自增主键
        DeviceId    NVARCHAR(20)        NOT NULL,       -- 设备 ID (ESP32 MAC)
        RecordTime  DATETIME2           NOT NULL,       -- 记录时间 (服务器收到时间)
        Temperature DECIMAL(5,1)        NULL,           -- 温度 (°C)
        Humidity    DECIMAL(5,1)        NULL,           -- 湿度 (%)
        Pressure    DECIMAL(7,1)        NULL,           -- 气压 (hPa)
        Altitude    DECIMAL(6,1)        NULL,           -- 海拔 (m)
        CreatedAt   DATETIME2           DEFAULT SYSDATETIME()  -- 入库时间
    );
END
GO

-- 按设备 + 时间查讯 (常用查询)
CREATE NONCLUSTERED INDEX IX_WeatherReadings_DeviceId_RecordTime
    ON dbo.WeatherReadings (DeviceId, RecordTime DESC);
GO

-- 查看最近 20 条 (快速验证)
-- SELECT TOP 20 * FROM dbo.WeatherReadings ORDER BY RecordTime DESC;
GO
