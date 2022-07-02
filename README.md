###### *Note that this is a 0.6 teeworlds mod (DDNet-compatible), if you are looking for 0.7 version, check [InfCroya](https://github.com/yavl/teeworlds-infcroya)*
###### *请注意这是个Teeworlds 0.6 Mod (DDNet-compatible)，如果您要0.7版本，请前往 [InfCroya](https://github.com/yavl/teeworlds-infcroya)*

# Teeworlds InfClassCR
修改自 [yavl 的 InfClassR](https://github.com/yavl/teeworlds-infclassR).
## 前置库
[GeoLite2++](https://www.ccoderun.ca/GeoLite2++/api/) 使用IP定位
```bash
sudo apt install libmaxminddb-dev
```

## 克隆库并构建
安装 [bam](https://github.com/matricks/bam) 0.4.0 构建工具，或者使用已编译好的 [bam](https://github.com/yavl/teeworlds-infclassR/tree/master/bin/bam) 来构建。
```
git clone https://github.com/yavl/teeworlds-infclassr
cd teeworlds-infclassr
```
然后把bam构建工具复制到teeworlds-infclassCR。记得把所有命令中的库url换成本库或者其它库。

### Ubuntu / Mint / Debian
```bash
sudo apt install libicu-dev libmaxminddb-dev
./bam server_debug
```
查看 [wiki](https://github.com/yavl/teeworlds-infclassR/wiki) 来了解更多。

### macOS 
via [Homebrew](https://brew.sh):
```bash
brew install icu4c libmaxminddb
./bam server_debug_x86_64
```

### Windows 
应当安装GCC编译器，例如[Mingw-w64](https://mingw-w64.org)。
```
bam config stackprotector=true geolocation=false
bam server_debug
```

## 服务器指令
查看原版[wiki](https://github.com/yavl/teeworlds-infclassR/wiki)
