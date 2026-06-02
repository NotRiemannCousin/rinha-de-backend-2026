# rinha-de-backend-2026

Esse repositório contem o código da minha participação na [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026). O projeto será feito em C++ usando a minha biblioteca de sockets, Hermes.



## Tentativas

Eu queria testar já de cara algumas otimizações em relação à representação dos dados. Infelizmente, depois de bater cabeça com alguns testes e pesquisas, eu cheguei à conclusão de que a arquitetura do servidor do projeto (Haswell, de um Mac Mini Late 2014) aparenta inviabilizar "truques baratos" como quantization (farei testes na maquina real quando for possivel). Pelo menos, ainda dá para normalizar todos os vetores para não ter que calculá-los na hora da comparação (em `normalization.py`).

Outra otimização, agr com relação a comunicação com a rede, é usar sockets UNIX com `SCM_RIGHTS` pra repassar o socket direto pro processo da API. Sinceramente eu nn sei até que nível isso é fair play afinal o docker estar em modo `bridge` é obrigatório então imagino que era pra gente fingir ser maquinas diferentes, mas se nn foi explicitado tá tudo bem né? Enfim kk.

Entrando nos detalhes técnicos da Hermes, ela é uma lib baseada em policies então o desenvolvimento desse socket com repasse fica +- assim:
- O load balancer customizado usa um `AsyncListenerSocket` cujo `SocketData` contém uma lista de sockets UNIX para cada instância da API. No `AsyncAcceptOne()`, o sender vai escolher uma instância da API usando round-robin e usar `sendmsg` com `SCM_RIGHTS` pra repassar pra instância correspondente (o socket retornado pelo `AsyncAcceptOne()` é descartado).
- De modo semelhante, cada instância da API contém um `AsyncListenerSocket` cujo `SocketData` contém o próprio socket UNIX. No `AsyncAcceptOne()`, é o `recvmsg` que vai ser chamado com `SCM_RIGHTS` e o socket retornado é uma cópia daquele que foi recebido inicialmente. É esse socket que a API vai utilizar pra responder diretamente pro client.


Tecnicamente isso só funciona pq estamos usando docker na mesma máquina e mapeando um volume compartilhado no `docker-compose.yml` (tipo `./sockets:/sockets`). Como os contêineres isolam o filesystem, sem esse volume o load balancer e a API não enxergariam o mesmo arquivo `.sock`. Se fossem máquinas físicas diferentes, o file descriptor transferido não faria sentido pro kernel remoto e eu precisaria fazer um proxy reverso TCP clássico e tal, oq seria bem triste de se fazer.



### 1 - Linear Search

Essa é a abordagem naive que vai servir de base para as minhas próximas tentativas. Um simples e singelo loop verificando todos os vetores e guardando os mais próximos, não é tão rápido mas pelo menos acerta 100% das vezes.

Pode parecer ruim (e realmente é, 3.000.000 de elementos é um número até que pequeno kkkk), porém o computador adora essa linearidade e pela misericórdia do compilador esse código com certeza será vetorizado.

Sobre memória, são 3.000.000 de elementos, cada um com 14 floats e 1 bool ("legit" ou "fraud") em 2 instância da API. Se a gente expremer tudo e desabilitar o padding, cabe tudo dentro dos limites definidos e com o load balancer customizado ainda sobra um pouqinho pro sistema processar as requests.

> 3.000.000 &times; (14 &times; 4 B + 1 B) &times; 2 = 6.000.000 &times; 57 B = 342.000.000 B &asymp; 326 MB

Bom, agr só falta implementar...


> TODO: Implementar o Linear Search

> TODO: Implementar o K-means

# ???

Esses só testando pra ver se vale a pena: Redução de dimensão e HNSW.