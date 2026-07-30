-- =============================================
-- 将 CSV 批量导入 MSSQL
-- 用 SQL Server Agent 定时任务或手动运行
-- =============================================
USE WeatherDB;
GO

BULK INSERT WeatherReadings
FROM 'D:\WeatherData\weather_log.csv'
WITH (
    FORMAT       = 'CSV',
    FIRSTROW     = 1,                               -- CSV 没有表头
    FIELDTERMINATOR = ',',
    ROWTERMINATOR = '\n',
    DATAFILETYPE = 'char',
    TABLOCK
);
GO

-- 导入后清空 CSV (移到备份)
-- EXEC xp_cmdshell 'move D:\WeatherData\weather_log.csv D:\WeatherData\archive\weather_log_%date:~0,4%%date:~5,2%%date:~8,2%.csv';
GO

-- 查看导入结果
SELECT TOP 20 * FROM WeatherReadings ORDER BY RecordTime DESC;
GO
