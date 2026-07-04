data = open('C:/Programs/mhook/source mishkinamish/x64/Release/mishkinamish.exe', 'rb').read()
text = data.decode('cp1251', errors='replace')
idx = text.find('Mishkina')
if idx > 0:
    print(text[idx:idx+60], file=open('C:/Programs/mhook/source mishkinamish/out.txt', 'w'))
else:
    print('Not found', file=open('C:/Programs/mhook/source mishkinamish/out.txt', 'w'))