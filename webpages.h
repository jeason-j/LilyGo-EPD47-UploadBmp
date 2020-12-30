const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
</head>
<body>
  <p><h1>设置墨水屏显示</h1>  </p>
  
  <p>可用: %FREESPIFFS% | 已用: %USEDSPIFFS% | 全部: %TOTALSPIFFS% <button onclick="rebootButton()">重启</button> <button onclick="sleepButton()">休眠</button> </p>
  <form method="POST" action="/upload" enctype="multipart/form-data"><input type="file" name="data"/><input type="submit" name="upload" value="Upload" title="Upload File"></form>
  <p>上传文件要求：<br/>
     1.分辨率长宽限制在 960*540内 <br/>
     2.色深 1,4,8,16位 <br/>
     3.黑白 bmp格式图片<br/>
     4.文件大小不建议超过1M </p>
  <p>%FILELIST%</p> 
  <p>
  <img src="showbmp">
  </p>
  <div id="status"> </div>
<script>


function rebootButton() {
  document.getElementById("status").innerHTML = "esp32 重启 ...";
  var xhr = new XMLHttpRequest();
  xhr.open("GET", "/reboot", true);
  xhr.send();
  window.open("/reboot","_self");
}

function sleepButton() {
  document.getElementById("status").innerHTML = "esp32 体眠 ...";
  var xhr = new XMLHttpRequest();
  xhr.open("GET", "/sleep", true);
  xhr.send();
  //window.open("/reboot","_self");
}


</script>
</body>
</html>
)rawliteral";


// reboot.html base upon https://gist.github.com/Joel-James/62d98e8cb3a1b6b05102
const char reboot_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html lang="en">
<head>
  <meta charset="UTF-8">
</head>
<body>
<h3>
  Rebooting, returning to main page in <span id="countdown">20</span> seconds
</h3>
<script type="text/javascript">
  var seconds = 20;
  function countdown() {
    seconds = seconds - 1;
    if (seconds < 0) {
      window.location = "/";
    } else {
      document.getElementById("countdown").innerHTML = seconds;
      window.setTimeout("countdown()", 1000);
    }
  }
  countdown();
</script>
</body>
</html>
)rawliteral";
