# Сайт проекта topiteneninglmsabce_и_гаяна

Статический сайт: описание, команда, скачивание клиента, гайд по Mistral API.

## Структура

```
website/
├── index.html
├── css/style.css
├── js/app.js
├── assets/              # логотипы (3 темы)
├── assets/guide/mistral/  # иллюстрации гайда (SVG; можно заменить на PNG)
└── releases/            # сюда — .exe и linux-бинарник
```

## Локальный просмотр

```bash
cd website
python3 -m http.server 8080
```

Откройте http://127.0.0.1:8080/

## Размещение на GitHub Pages

1. Закоммитьте папку `website/` (без push — по желанию).
2. На GitHub: **Settings → Pages**.
3. **Source:** Deploy from a branch.
4. Если доступна только корневая папка `/` — используйте workflow ниже или перенесите содержимое в `docs/`.

### Вариант с GitHub Actions (папка `website/`)

Создайте `.github/workflows/pages.yml`:

```yaml
name: Deploy website
on:
  push:
    branches: [main, master]
    paths: ['website/**']
permissions:
  pages: write
  id-token: write
jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/upload-pages-artifact@v3
        with:
          path: website
      - uses: actions/deploy-pages@v4
```

В **Settings → Pages** выберите **GitHub Actions** как источник.

Сайт будет по адресу: `https://<username>.github.io/<repo>/`

## Размещение на сервере вуза

Скопируйте **всё содержимое** папки `website/` на сервер (FTP/SFTP/scp), например в `~/public_html/`:

```bash
scp -r website/* user@server.university.ru:~/public_html/
```

Относительные пути (`css/`, `releases/`) менять не нужно.

## Релизы клиента

1. Положите файлы в `website/releases/`, **или**
2. GitHub Releases → прикрепите `.exe` и linux-бинарник → обновите `href` кнопок в `index.html`.

Пока файлов нет, кнопки помечены «скоро».

## Скриншоты Mistral

Замените SVG в `assets/guide/mistral/` на PNG с теми же именами:

- `01-mistral-home.png`
- `02-console-login.png`
- `03-api-keys-menu.png`
- `04-create-key.png`
- `05-copy-key.png`

Сайт подхватит PNG автоматически, если файлы лежат рядом с SVG.

**Не публикуйте на скриншотах настоящий API-ключ.**

## Темы

Три темы: **Светлая**, **Тёмная** (как в GUI) и **ИУ5** (тема кафедры, как в GUI). Выбор сохраняется в `localStorage`.

Видео для вкладки «ГОООООЛ»: `assets/goal.mp4`.
