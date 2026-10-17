# form_url — 表单/Cookie 解析宿主单测(**不参与固件构建**)

被测源码是 **`main/form_url.c`**(固件也在用同一份),所以这里通过即代表端上行为,
不存在"参考副本漂移"。

## 为什么需要

强制门户登录(1.3版本优化.md **1.3-01**)依赖两段**纯文本解析**:

| 函数 | 风险点 |
| --- | --- |
| `app_url_decode` | `+`→空格、`%XX`→字节;**非法/不足两位的转义必须原样保留**(否则用户口令被静默改写) |
| `app_form_lookup` | 值为空串要算命中;值里含 `=`(`password=a=b`)要保留;重复键取首个 |
| `app_cookie_value` | 键名边界(**`xiothub_sid=` 不能命中 `iothub_sid`**);`;` 分隔与尾随分号 |

这些错误在真机上的表现只是"偶尔登录失败",极难定位 → 用单测钉死。

## 运行单测

```bash
gcc -Wall -Wextra -I main -o /tmp/t_form_url \
    tools/form_url/test_form_url.c main/form_url.c && /tmp/t_form_url
# 期望:结果: 共 36 例, 失败 0
```

## 约束

`form_url.h/.c` **刻意保持零 ESP-IDF 依赖**(仅 `ctype.h` / `stdlib.h` / `string.h` /
`stdbool.h` / `stddef.h`),就是为了让本单测能直接编译源码。**请勿在其中引入 IDF 头文件**
(也不要改用 `app_copy_string()`,那会把 `common.h`/FreeRTOS 拉进来)。
