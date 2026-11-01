// Большая часть содрана из WAVStream, а тот из GZSamplesHolder2
#include <Windows.h>
#include <math.h>
#include <stdio.h>
#include "MMGlobals.h"
#include "WAVLoader.h"
extern bool f_reading_file;  // определено в InputThread.cpp, хотя красивее было
                             // бы сделать её атрибутом класса, но лень уже.
float super_prev = 0.0f;  // Используется для pre-emphasis во время паузы
// Пауза и медленное передвижение в файле
bool flag_pause = false;
bool flag_key_left = false;
bool flag_key_right = false;
// новое-блестящее
short *samples = NULL;  // Здесь будут загруженные из файла семплы
long MMWAVLoader::position = 0;
long MMWAVLoader::length = 0;
// Переменные для диалога
#ifdef MMISH_ENGLISH
static TCHAR *filter_WAV = L"WAV files\0*.WAV\0\0";
#else
static TCHAR *filter_WAV = L"файлы WAV\0*.WAV\0\0";
#endif
static TCHAR filename[1258];
static TCHAR filetitle[1258];
// Вспомогательный класс заголовка WAV-файла
class GZ_WAVHeader {
 public:
  char RIFF[4];
  long RIFFsize;
  char WAVEfmt[8];   // Содержит строку "WAVEfmt "
  long WAVEsize;     // Для PCM здесь 16
  short wFormatTag;  // 1 для PCM, то есть без компрессии
  unsigned short wChannels;
  unsigned long dwSamplesPerSec;
  unsigned long dwAvgBytesPerSec;
  unsigned short wBlockAlign;
  unsigned short wBitsPerSample;
  // Замечание: здесь могут быть дополнительные поля, в зависимости от
  // wFormatTag.
  char DATA[4];
  unsigned long DATAsize;
};
TCHAR *MMWAVLoader::LoadWavFile(TCHAR *_filename, HWND hdwnd) {
  GZ_WAVHeader wh;
  TCHAR error_msg[1024];
  int _num_channels, _num_bits_per_sample, _num_samples_per_sec, _num_samples;
  // Если имя файла задано, диалог не выводим
  if (_filename) {
    wcscpy_s(filename, _filename);
    wcscpy_s(filetitle, _filename);
  } else {
    // 0. Показываем диалог
    OPENFILENAME ofn = {sizeof(OPENFILENAME),
                        NULL,
                        NULL,  // в данном конкретном случае игнорируется
                        filter_WAV,
                        NULL,
                        0,  // Не используем custom filter
                        0,  // -"-
                        filename,
                        256,
                        filetitle,
                        256,
                        NULL,
                        L"Открыть файл WAV",
                        OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                        0,
                        0,
                        L"WAV",
                        0,
                        0,
                        0};
    // Диалог запроса имени файла
    if (0 == GetOpenFileName(&ofn)) {
      return NULL;
    }
  }  // вывод диалога
  FILE *fin = NULL;
  _wfopen_s(&fin, filename, L"rb");
  // 1. Открываем файл в binary mode
  if (NULL == fin) {
    wcscpy_s(error_msg, L"Не могу открыть файл '");
    wcsncat_s(error_msg, filename, 1000);
    wcsncat_s(error_msg, L"'", 2);
    // GZReportError(error_msg);
    MessageBox(hdwnd, error_msg, L"Ошибка", MB_OK);
    return NULL;
  }
  // 2. Считываем заголовок и проверяем его на вшивость
  if (1 != fread(&wh, sizeof(GZ_WAVHeader), 1, fin)) {
    goto bad_format;
  }
  if ((0 != strncmp(wh.RIFF, "RIFF", 4))
      || (0 != strncmp(wh.WAVEfmt, "WAVEfmt ", 8)) || (16 != wh.WAVEsize)
      || (1 != wh.wFormatTag) || ((1 != wh.wChannels) && (2 != wh.wChannels))
      || ((16 != wh.wBitsPerSample) && (8 != wh.wBitsPerSample))
      //		(2!=wh.wBlockAlign)||
  ) {
    goto bad_format;
  }
  // 3. Проматываем все чанки, пока не напоремся на чанк "data"
  while (0 != strncmp(wh.DATA, "data", 4)) {
    // Проверяем, что файл не окончательно испорчен, и что имя чанка осмыслено
    // Пока знаю только про чанки JUNK и LIST. Попадутся новые - добавлю
    if ((0 != strncmp(wh.DATA, "JUNK", 4))
        && (0 != strncmp(wh.DATA, "LIST", 4)))
      goto bad_format;
    fseek(fin, wh.DATAsize, SEEK_CUR);  // проматываем
    fread(&wh.DATA, 8, 1, fin);  // читаем заголовок следующего чанка
  }
  // 4. Загрузка отсчетов
  _num_channels = wh.wChannels;
  _num_bits_per_sample = wh.wBitsPerSample;
  _num_samples_per_sec = wh.dwSamplesPerSec;
  // Безопасное вычисление числа сэмплов с проверкой переполнения
  if (_num_bits_per_sample == 0 || _num_channels == 0) goto bad_format;
  if (wh.DATAsize > 0x7FFFFFFFU) goto bad_format;  // слишком большой файл
  _num_samples = (long)((unsigned long long)wh.DATAsize * 8 / _num_bits_per_sample / _num_channels);
  if (_num_samples <= 0) goto bad_format;
  if (16 != _num_bits_per_sample) goto bad_format;     // только 16 бит
  if (16000 != _num_samples_per_sec) goto bad_format;  // только 16 кГц
  if (1 != _num_channels) goto bad_format;             // только моно
  // Проверка на разумный размер файла (не более 500 МБ)
  if (wh.DATAsize > 500 * 1024 * 1024) goto bad_format;
  // собственно, загрузка
  // грузим в массив samples
  if (samples) delete[] samples;
  // Чанк data может иметь нечётный размер (байт-выравнивание в конце) —
  // его отбрасываем, чтобы не прочитать лишний байт за массивом
  samples = new short[_num_samples];
  if ((size_t)_num_samples != fread(samples, sizeof(short), _num_samples, fin))
    goto bad_format;
  fclose(fin);
  length = _num_samples;  // Потом используется в FillBuffer;
  position = 0;
  return filetitle;  // Можно вписать его в заголовки окон
bad_format:
  if (samples) {
    delete[] samples;
    samples = 0;
  }
  length = 0;
  fclose(fin);
  position = 0;
#ifdef MMISH_ENGLISH
  MessageBox(hdwnd,
             L"Unsupported file format. Use only 16 bit / 16 kHz / mono files",
             L"Error",
             MB_OK);
#else
  MessageBox(hdwnd,
             L"Неподдерживаемый формат файла. Используйте только 16 бит / 16 "
             L"кГц / моно файлы",
             L"Ошибка",
             MB_OK);
#endif
  // GZReportError(L"Неподдерживаемый формат файла");
  return (NULL);
};
//==================================================
// движение влево вправо по остановленному звуку
//==================================================
void MMWAVLoader::snd_left_right() {
  if (flag_key_left) {
    position -= 160;
    flag_key_left = false;
  }
  if (flag_key_right) {
    position += 160;
    flag_key_right = false;
  }
  if (position < 0) position = 0;
  if (length > 0 && position >= length)
    position = position % length;
}
//===================================================================================
// Копирует фрейм из загруженных семплов. Если дошёл до конца, переходит в
// начало
//===================================================================================
void MMWAVLoader::FillBuffer(short *input_buf) {
  if (!f_reading_file)
    return;  // во время смены одного файла на другой samples может быть
             // неправильный
  if (!samples || length <= 0)
    return;  // защита от нулевого буфера
  long copied = 0;
  long start = position;
  // Кадр мог упереться в конец файла, тогда остаток добираем с его начала.
  // Файл может быть и короче кадра - поэтому заворачиваемся сколько раз нужно.
  while (copied < MM_SOUND_BUFFER_LEN) {
    if (start >= length) start = 0;
    long chunk = MM_SOUND_BUFFER_LEN - copied;
    if (chunk > length - start) chunk = length - start;
    CopyMemory(input_buf + copied, &samples[start], chunk * sizeof(short));
    copied += chunk;
    start += chunk;
  }
  // Возможна пробуксовка
  if (flag_pause) {
    if (position > 0)
      super_prev = (float)samples[position - 1];
    else
      super_prev = 0.0f;
    snd_left_right();  // не подвинуться ли?
  } else {
    position += MM_SOUND_BUFFER_LEN;
    if (position >= length) position %= length;
  }
}