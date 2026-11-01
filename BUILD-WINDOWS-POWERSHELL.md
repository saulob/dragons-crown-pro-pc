# Build completo do Dragon's Crown Pro PC no Windows

Este arquivo compila o fork `dragons-crown-pro-new-interface` com a nova interface v0.7 integrada. O build oficial do projeto usa Windows 10/11 de 64 bits, MSYS2 CLANG64, CMake e Ninja.

## 1. Pré-requisitos

Abra o PowerShell normal como usuário. Não é necessário executar como administrador para compilar.

Se o Windows perguntar individualmente se deve executar cada script, desbloqueie os scripts uma vez na pasta do projeto:

```powershell
Get-ChildItem -Recurse -File | Unblock-File
```

Depois disso, os comandos `setup.ps1`, `build.ps1` e `package.ps1` não devem pedir confirmação para cada arquivo.

Instale o Git e o MSYS2:

```powershell
winget install --id Git.Git -e
winget install --id MSYS2.MSYS2 -e
```

Feche e abra o PowerShell novamente. Confirme que o MSYS2 foi instalado em `C:\msys64`. Se estiver em outro local, altere `$msys` nos comandos abaixo.

```powershell
$msys = 'C:\msys64'
Test-Path "$msys\usr\bin\bash.exe"
```

O último comando deve retornar `True`.

## 2. Atualizar o MSYS2

Execute os comandos abaixo no PowerShell. O primeiro update pode pedir para fechar o terminal; se isso acontecer, feche o PowerShell, abra-o novamente e execute o segundo comando.

```powershell
$msys = 'C:\msys64'
& "$msys\usr\bin\bash.exe" -lc 'pacman -Syu --noconfirm'
& "$msys\usr\bin\bash.exe" -lc 'pacman -Su --noconfirm'
```

## 3. Instalar o toolchain CLANG64

Ainda no PowerShell:

```powershell
$packages = @(
  'mingw-w64-clang-x86_64-toolchain',
  'mingw-w64-clang-x86_64-cmake',
  'mingw-w64-clang-x86_64-ninja',
  'mingw-w64-clang-x86_64-pkgconf',
  'mingw-w64-clang-x86_64-sdl3',
  'mingw-w64-clang-x86_64-freetype',
  'mingw-w64-clang-x86_64-vulkan-headers',
  'mingw-w64-clang-x86_64-vulkan-loader',
  'mingw-w64-clang-x86_64-vulkan-memory-allocator',
  'mingw-w64-clang-x86_64-glslang',
  'mingw-w64-clang-x86_64-fmt',
  'mingw-w64-clang-x86_64-boost',
  'mingw-w64-clang-x86_64-robin-map',
  'mingw-w64-clang-x86_64-xxhash',
  'mingw-w64-clang-x86_64-libpng',
  'mingw-w64-clang-x86_64-zydis',
  'mingw-w64-clang-x86_64-spirv-tools',
  'mingw-w64-clang-x86_64-spirv-headers',
  'mingw-w64-clang-x86_64-spirv-cross'
)
$packageText = $packages -join ' '
& "$msys\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm $packageText"
```

## 4. Obter o código-fonte completo

```powershell
$source = Join-Path $HOME 'src\dragons-crown-pro-new-interface'
New-Item -ItemType Directory -Force (Split-Path $source) | Out-Null
if (Test-Path $source) {
  Write-Host "A pasta já existe: $source"
} else {
  git clone https://github.com/vuvandro/dragons-crown-pro-new-interface.git $source
}
Set-Location $source
```

Se você recebeu este projeto como ZIP, extraia o ZIP e use o caminho extraído no lugar de `$source`.

## 5. Aplicar/confirmar a nova interface

O ZIP entregue já contém os arquivos modificados e adicionados. Para conferir:

```powershell
Get-ChildItem .\launcher\ui\src
Get-ChildItem .\launcher\ui\include\trg
Get-Content .\launcher\launcher.cpp | Select-String 'RowLarge|Keyboard controls|Game files'
```

Os pontos principais são:

- `launcher\ui\src\launcher.cpp`: renderer visual da v0.7;
- `launcher\ui\include\trg\`: API e componentes da interface;
- `launcher\ui\fonts\`: Inter, Cinzel e JetBrains Mono;
- `launcher\launcher.cpp`: abas, Game files, versões suportadas e Controls;
- `launcher\CMakeLists.txt`: integração do renderer e das fontes.

## 6. Verificar dependências

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& .\tools\setup.ps1
```

O resultado esperado é `Ready.`. Python é opcional para compilar; só é necessário para as ferramentas de preparação de PKG.

## 7. Compilar o executável Windows

O comando abaixo configura o CMake, sincroniza as dependências do vídeo e compila `dragons_crown_pro.exe` e `trg-updater.exe`:

```powershell
& .\tools\build.ps1
```

Os executáveis serão gerados em:

```text
build\cmake\clang64\bin\dragons_crown_pro.exe
build\cmake\clang64\bin\trg-updater.exe
```

Para uma recompilação limpa:

```powershell
& .\tools\build.ps1 -Clean
```

## 8. Gerar o ZIP distribuível com o EXE e DLLs

Depois de o build terminar sem erros:

```powershell
& .\tools\package.ps1 -SkipBuild
```

O pacote final será criado em:

```text
dist\DragonsCrownProPC-v0.9.0-windows-x64.zip
```

O diretório temporário também ficará disponível em:

```text
dist\DragonsCrownProPC-v0.9.0-windows-x64\
```

O ZIP inclui o executável, `trg-updater.exe`, as DLLs necessárias, README, licença e os assets da interface. Ele não inclui arquivos de jogo, saves, `.pkg` ou dados pessoais.

## 9. Testar o launcher

Para abrir o launcher a partir do código compilado:

```powershell
& .\tools\build.ps1 -Run
```

Na aba **Play**, use **Install game...** e selecione o seu próprio pacote `.pkg` legalmente obtido. O pacote do jogo não faz parte deste código.

## 10. Onde ficam os saves

O launcher usa a pasta `user` ao lado do executável. Para fazer backup, copie a pasta inteira:

```text
user\savedata\
user\dcp.ini
user\trophy\
user\keys.json
```

Copiar a pasta `user` é suficiente para preservar saves, configurações e troféus.

## Solução de problemas

### `MSYS2 not found`

Defina o caminho antes de executar o build:

```powershell
$env:MSYS2_ROOT = 'C:\msys64'
& .\tools\setup.ps1
```

### `std::abort` não existe em `sirit/src/stream.h`

O pacote completo já contém a correção de portabilidade para versões atuais do MSYS2/Clang: `stream.h` inclui `<cstdlib>`. Se você estiver usando uma cópia anterior do ZIP, atualize o arquivo para que exista esta linha junto dos outros includes C++:

```cpp
#include <cstdlib>
```

Depois limpe e compile novamente:

```powershell
Remove-Item -Recurse -Force .\build\cmake\clang64 -ErrorAction SilentlyContinue
& .\tools\build.ps1
& .\tools\package.ps1 -SkipBuild
```

### `common_initialized` foi definido mas não foi usado

O código completo atualizado já remove esse estado global sem uso em `src\runtime_services.c`. Se a sua cópia ainda apresentar:

```text
variable 'common_initialized' set but not used [-Werror,-Wunused-but-set-global]
```

extraia o ZIP corrigido novamente e faça uma compilação limpa:

```powershell
Remove-Item -Recurse -Force .\build\cmake\clang64 -ErrorAction SilentlyContinue
& .\tools\build.ps1
& .\tools\package.ps1 -SkipBuild
```

### `CLANG64 incomplete`

Execute novamente a instalação de pacotes da seção 3 dentro do PowerShell. Depois abra um novo PowerShell e rode:

```powershell
& .\tools\setup.ps1
& .\tools\build.ps1
```

### O jogo não está instalado

Isso não impede a compilação. O launcher exibirá a mensagem para instalar o jogo na aba Play. A instalação acontece depois, através de **Install game...**.

### O antivírus bloqueia o executável

O projeto gera um executável local sem assinatura digital. Adicione a pasta de build à exceção somente se você confia no código e compilou a partir da fonte correta.
