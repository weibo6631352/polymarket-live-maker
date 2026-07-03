# 预编译 Fast-DDS 动态库(入 git,消除部署机的 DDS 源码编译)

`fastdds-<版本>-<os>-<arch>.tar.gz` = 完整安装前缀(`include/ lib/ lib64/ share/`,`.so` 已
strip)。`WITH_TELEMETRY=ON` 时 CMake 自动匹配 `<CMAKE_SYSTEM_NAME>-<CMAKE_SYSTEM_PROCESSOR>`
(小写)解包到 `build/fastdds-prebuilt/` 并接入 `CMAKE_PREFIX_PATH` —— 盒子上那套 292M、数十分钟
的 `~/dds-build` 源码构建只在**升级 Fast-DDS 版本**时才需要。

当前包:`fastdds-3.6.1-linux-x86_64.tar.gz`(fastdds 3.6.1 + fastcdr 2.4.0 + foonathan_memory
0.7.4;外部依赖仅系统 libssl.so.3/libz;2026-07-03 由盒子 `~/dds` 打包)。

配套:`cpp/telemetry/generated/` = fastddsgen 从 `telemetry.idl` 生成的源码(与库同版本),默认
直接编译 —— 无 Java/Fast-DDS-Gen 依赖。改了 idl 才 `-DTELEMETRY_REGEN=ON` 重生成(输出直接写回
generated/,提交之)。

## 重建/升级包(在目标平台上)

```sh
# 1) 源码构建 Fast-DDS 到某前缀 (一次性; 见 eProsima 文档, colcon 或逐仓库 cmake --install)
# 2) 打包 (strip 副本, 不动原前缀):
mkdir -p /tmp/ddspkg/prefix && cp -a $PREFIX/{include,lib,lib64,share} /tmp/ddspkg/prefix/
find /tmp/ddspkg/prefix -name '*.so*' -type f -exec strip --strip-unneeded {} \;
tar -C /tmp/ddspkg/prefix -czf fastdds-<ver>-linux-x86_64.tar.gz .
```

Mac 本地默认 `WITH_TELEMETRY=OFF`(no-op publisher,零依赖),故只提供 linux-x86_64;需要 Mac
遥测时按上法产 `fastdds-<ver>-darwin-arm64.tar.gz` 放进本目录即可,CMake 自动匹配。
