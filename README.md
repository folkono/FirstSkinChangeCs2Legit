# FirstSkinChangeLegit

FirstSkinChangeLegit é um projeto experimental de **viewmodel externo para CS2**. Ele captura a imagem exibida na tela, identifica a região ocupada pela arma original, reconstrói o fundo nessa área e desenha outro modelo em uma overlay. O programa não lê nem altera a memória do jogo.

O resultado visual já é satisfatório: a arma acompanha a cena, recebe luz e reflexos do mapa e, em vários momentos, parece fazer parte do jogo. O principal ponto a melhorar ainda é a **sincronia da máscara** com as animações e os movimentos do viewmodel original. Quando ela se adianta ou atrasa, partes da arma original podem aparecer.

## Como funciona

1. Captura os quadros da tela e acompanha os inputs configurados.
2. Obtém a máscara do viewmodel a partir das gravações de tela verde e, quando disponível, de um modelo de segmentação ONNX.
3. Usa quadros anteriores e NVIDIA Optical Flow para tentar reconstruir o fundo oculto pela arma. Onde não há informação confiável, aplica o preenchimento alternativo.
4. Renderiza o modelo escolhido em uma overlay Direct3D 11, com animações, iluminação e reflexos derivados da imagem do jogo.

O menu abre com **HOME** por padrão. Atalhos, resolução, proporção da imagem, barras pretas, posição da arma e outras opções podem ser ajustados nele.

## Estado do projeto

É um protótipo em desenvolvimento, principalmente na sincronização entre captura, máscara e animação. A aparência pode variar conforme resolução, FPS, iluminação, gravação usada para a máscara e configuração dos binds. Não há integração oficial com o CS2.

## Compilar e executar

Requisitos: Windows, Visual Studio 2022 Build Tools com C++, CMake e GPU compatível com Direct3D 11. Os componentes de terceiros usados pelo projeto estão em `native/third_party`, com seus respectivos avisos de licença.

Execute `native\build.bat`. Depois, inicie `native\build\Release\vmoverlay.exe`. A interface de visualização no navegador pode ser aberta com `abrir.bat`.

O ZIP público contém **apenas código e dependências de terceiros**. Ele não inclui modelos, texturas, animações, capturas, máscaras, gravações ou pesos de IA do jogo. Para ver um viewmodel, forneça arquivos que você tenha direito de usar nos diretórios esperados pelo código (`models/view`, `models/gloves`, `models/cs2vm` e `textures`). Os catálogos `native/skins.tsv` e `skins.json` começam vazios no pacote público.

### Fontes usadas durante o desenvolvimento

O histórico de desenvolvimento registra estas origens para os assets usados na versão local:

- [CS2 Spraylab](https://spraylab.pages.dev/): viewmodels em `models/view`, luvas em `models/gloves`, texturas em `textures/cosmetics` e dados usados no catálogo de skins.
- [AstraStrike](https://astrastrike.fun/): animações de viewmodel usadas em `models/cs2vm`.

Esses arquivos **não acompanham este repositório**. A indicação das fontes não concede licença para baixar, usar ou redistribuir os assets; confira as permissões aplicáveis antes de utilizá-los. FirstSkinChangeLegit não tem ligação oficial com a Valve, o CS2 ou esses sites.

## Capturas

Imagens de funcionamento sem redistribuir os arquivos de modelo e textura.<br>
https://github.com/user-attachments/assets/11235c22-39e0-4a5f-b5a5-4876e8672f0b

## Licenças

Os avisos das bibliotecas de terceiros acompanham seus arquivos. A licença do código próprio do projeto ainda precisa ser definida antes da publicação.
