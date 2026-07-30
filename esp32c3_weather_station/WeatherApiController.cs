using Microsoft.AspNetCore.Mvc;

[ApiController]
[Route("api/[controller]")]
public class WeatherController : ControllerBase
{
    // CSV 文件路径（确保这个目录存在）
    private const string CsvPath = @"D:\WeatherData\weather_log.csv";

    [HttpPost]
    public IActionResult Post([FromBody] WeatherData data)
    {
        try
        {
            // 确保目录存在
            var dir = System.IO.Path.GetDirectoryName(CsvPath);
            if (!System.IO.Directory.Exists(dir))
                System.IO.Directory.CreateDirectory(dir);

            // 追加一行 CSV (不加表头, BULK INSERT 时指定)
            string line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss},{data.Device},{data.Temp:F1},{data.Humidity:F1},{data.Pressure:F1},{data.Altitude:F1}";
            System.IO.File.AppendAllText(CsvPath, line + Environment.NewLine);

            Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {data.Device} -> CSV OK");

            return Ok(new { status = "ok" });
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[ERROR] {ex.Message}");
            return StatusCode(500, new { error = ex.Message });
        }
    }
}

public class WeatherData
{
    public string Device { get; set; }
    public float Temp { get; set; }
    public float Humidity { get; set; }
    public float Pressure { get; set; }
    public float Altitude { get; set; }
}
