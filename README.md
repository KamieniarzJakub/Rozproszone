TODO:
* parametryzacja zapytań
* zwiększenie różnicy czasu - modyfikacja rand, tak aby kolejne wątki się bardziej desynchronizowały (np. poprzez uśpienie danego wątku na pewien dłuższy czas)


Opis problemu: <br/>
Grupa emerytek bardzo chciałaby robić konfitury dla wnuczek, tylko że niestety wnuczek nie mają. Na szczęście grupa studentek Uniwersytetu Artystycznego chciałaby, by ktoś je dokarmiał - więc wspólnie emerytki i studentki założyły stowarzyszenie, gdzie babcie tworzą konfitury, a studentki je zjadają, po czym oddają słoiki emerytkom.

Procesy: B babć i S studentek<br/>
Zasoby: P słoików, K konfitur - ale P i K to wartości maksymalne<br/>
Wymogi: Babcie ubiegają się o słoiki. Gdy mają słoik, tworzą konfiturę. Studentki ubiegają się o konfitury. Gdy dostaną konfiturę, zjadają ją i oddają słoik.<br/>

Procesy działają z różną prędkością, mogą wręcz przez jakiś czas nie chcieć robić albo jeść konfitur. Nie powinno to blokować pracy innych procesów.

Założenia początkowe:<br/>
P=K (liczba słoików nie może być większa niż liczba konfitur - z każdego słoika babcia jest w stanie stworzyć smaczną konfiturę, niezależnie od liczby konfitur jaka już powstała. Moja babcia potwierdziła. Analogicznie, z gotowej konfitury można zrobić pusty słoik, a zwłaszcza jak jest smaczna).<br/>
Stan początkowy:<br/>
p - aktualna liczba dostępnych (pustych) słoików; p=P=K<br/>
k - aktualna liczba dostępnych konfitur; k=0<br/>


Procesy biorą zasoby pojedynczo, tzn. jedna studentka może w danym momencie jeść tylko jedną konfiturę, a babcia przygotowywać tylko jedną konfiturę.

Słoiki są nierozróżnialne pomiędzy sobą. Konfitury również są nierozróżnialne pomiędzy sobą.

Studentki jak usłyszały o inicjatywie, były bardzo zadowolone i głodne. Dozwolone jest, aby wysyłały prośbę o przydział konfitury do pozostałych studentek, zanim konfitury zostaną wytworzone (pod warunkiem, że aktualnie nie posiadają żadnej). W celu uzgodnienia kolejności otrzymywania zasobów wykorzystany zostanie algorytm Ricarta-Agrawali - dodatkowym warunkiem wysłania ACK jest to, że w ether pójdzie plota, że pojawiła się nowa konfitura lub wiadomym jest, że takowa jest w danym momencie dostępna. Procesy wiedzą na podstawie kolejki do kogo wysłać ACK w pierwszej kolejności (Zegar Lamporta + ranga). Zwalnianie zasobu krytycznego (słoik bez konfitury to naprawdę krytyczna sytuacja) jest ogłaszane tylko dla babć, które służą ratunkiem z opresji.


Babcie potraktowały to wyzwanie niezwykle poważnie, powszechną wiedzą jest, że niezwykłym zaszczytem jest stworzenie konfitury, dlatego babcie czasem walczą o kolejność. Tutaj analogicznie zdecydowały się ostatecznie uzgodnić kolejkę za pomocą algorytmu Ricarta-Agrawali (Zegar Lamporta, potem ranga). Studentki podeślą im SMS, jak dany słoik się zwolni (“Release” w formie broadcastu zdarzenia E do babć)

Gdy odpowiedni proces ma już zasób na wyłączność (babcia - słoik, studentka - konfiturę), to jego zużyciu rozgłasza wiadomość do wszystkich procesów drugiej grupy. Każdy z tych procesów utrzymuje odpowiednie liczniki zasobu na który czeka.
