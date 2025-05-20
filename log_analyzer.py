import re

# Ścieżka do pliku wejściowego i wyjściowego
input_file = 'log.txt'
output_file = 'log_in_order.txt'

# Wczytaj dane z pliku
with open(input_file, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Wyciągnij pary (linia, liczba) z drugiego nawiasu kwadratowego
pattern = re.compile(r'\[(.*?)\]\[(\d+)\]')

parsed = []
for line in lines:
    match = pattern.search(line)
    if match:
        value = int(match.group(2))
        parsed.append((line.strip(), value))

# Posortuj po liczbie
sorted_lines = sorted(parsed, key=lambda x: x[1])

# Zapisz posortowane linie do pliku
with open(output_file, 'w', encoding='utf-8') as f:
    for line, _ in sorted_lines:
        f.write(line + '\n')
