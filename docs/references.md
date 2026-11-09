# Полезные статьи и ссылки

Список литературы к [bot_design.md](bot_design.md). Пополняется по ходу работы.

## Игровые движки и поиск

| Работа | Чем полезна |
| --- | --- |
| В. Черепенников. [О шахматах. И не только](https://habr.com/ru/companies/huawei/articles/530892/) (Хабр, 2020) | «быстрые» и «умные» программы, AlphaZero, обратная задача ИИ — извлечение знания из сети |
| Silver et al. [Mastering Chess and Shogi by Self-Play with a General Reinforcement Learning Algorithm](https://arxiv.org/abs/1712.01815) (AlphaZero, 2017) | MCTS, управляемый нейросетью; самоигра |
| Danihelka et al. [Policy Improvement by Planning with Gumbel](https://mlanthology.org/iclr/2022/danihelka2022iclr-policy) (ICLR 2022) | Gumbel AlphaZero: обучение при малом числе симуляций |
| Antonoglou et al. [Planning in Stochastic Environments with a Learned Model](https://mlanthology.org/iclr/2022/antonoglou2022iclr-planning) (Stochastic MuZero, ICLR 2022) | узлы случая в MCTS |
| [Expectiminimax](https://en.wikipedia.org/wiki/Expectiminimax) (Википедия) | перебор с узлами случая |
| Veness, Blair. [Effective Use of Transposition Tables in Stochastic Game Tree Search](https://cgi.cse.unsw.edu.au/~blair/pubs/2007VenessBlairCIG.pdf) (CIG 2007) | Star1/Star2 и таблицы транспозиций в стохастических играх |
| Tesauro. [Temporal Difference Learning and TD-Gammon](https://bkgm.com/articles/tesauro/tdl.html) (1995) | мелкий перебор + обученная оценка в игре со случайностью |
| [Computer Arimaa](https://en.wikipedia.org/wiki/Computer_Arimaa) (Википедия) | игра с 4 шагами за ход; движки и их методы |
| J. Haskin. [A Look at the Arimaa Branching Factor](https://arimaa.janzert.com/bf_study) | ветвление при многошаговом ходе, транспозиции |

## Игры с ходом из многих действий

| Работа | Чем полезна |
| --- | --- |
| Justesen et al. [Online Evolution for Multi-Action Adversarial Games](https://pure.itu.dk/en/publications/online-evolution-for-multi-action-adversarial-games/) | эволюция хода целиком против статической оценки (Hero Academy) |
| Justesen et al. [Playing Multi-Action Adversarial Games: Online Evolutionary Planning versus Tree Search](https://pure.itu.dk/da/publications/playing-multi-action-adversarial-games-online-evolutionary-planni/) | сравнение OEP и вариантов MCTS |
| Ontañón. [Combinatorial Multi-armed Bandits for Real-Time Strategy Games](https://arxiv.org/abs/1710.04805) (JAIR) | Naive MCTS: факторизация комбинаторного действия |
| Justesen et al. [Blood Bowl: The Next Board Game Challenge for AI](https://pure.itu.dk/en/publications/blood-bowl-the-next-board-game-challenge-for-ai/) | самый близкий аналог нашей игры |
| [MimicBot: Combining Imitation and Reinforcement Learning to win in Bot Bowl](https://arxiv.org/abs/2108.09478) | победитель соревнования Bot Bowl |

## Футбольные игры и симуляторы

| Работа | Чем полезна |
| --- | --- |
| [EA SPORTS FC 25 — FC IQ Deep Dive](https://www.ea.com/en/games/ea-sports-fc/fc-25/news/pitch-notes-fc-25-fc-iq-deep-dive) | роли игроков, формация, командные установки |
| [FIFA 14 — Teammate Intelligence](https://www.ea.com/games/fifa/amp/news/fifa-14-teammate-intelligence) | поведение партнёров без мяча |
| [FM21 Match Engine AI Improvements](https://www.fmscout.com/a-fm21-match-engine-ai-improvements.html) | решения игроков в Football Manager: передача опеки, перекрытие линий паса |
| [HELIOS2013 Team Description Paper](https://staff.fnwi.uva.nl/a.visser/activities/robocup/RoboCup2013/Symposium/TeamDescriptionPapers/SoccerSimulation/Soccer2D/TDP_HELIOS2013.pdf) | RoboCup 2D: поиск цепочек действий владельца, прогноз соперников; формация по триангуляции Делоне |
| Kurach et al. [Google Research Football: A Novel Reinforcement Learning Environment](https://arxiv.org/abs/1907.11180) | среда RL для футбола, базовые алгоритмы |
| Liu et al. [From Motor Control to Team Play in Simulated Humanoid Football](https://arxiv.org/abs/2105.12196) | децентрализованные обученные агенты, 2 на 2 |
| [Tikick: Towards Playing Multi-agent Football Full Games From Single-agent Demonstrations](https://awesomepapers.io/reinforcement-learning/papers/huang2021tikick) | многоагентный футбол из демонстраций одного агента |

## Футбольная аналитика

| Работа | Чем полезна |
| --- | --- |
| Fernández, Bornn, Cervone. [A framework for the fine-grained evaluation of the instantaneous expected value of soccer possessions](https://arxiv.org/abs/2011.09426) (Machine Learning, 2021) | EPV: ценность владения, разложенная по действиям |
| Fernández, Bornn, Cervone. [Decomposing the Immeasurable Sport](https://www.sloansportsconference.com/research-papers/decomposing-the-immeasurable-sport-a-deep-learning-expected-possession-value-framework-for-soccer) (Sloan 2019) | EPV, ранняя версия |
| Fernández, Bornn. [Wide Open Spaces: A statistical technique for measuring space creation in professional soccer](https://sloansportsconference.com/research-papers/wide-open-spaces-a-statistical-technique-for-measuring-space-creation-in-professional-soccer) (Sloan 2018) | создание пространства — ценность движения без мяча |
| Decroos et al. [Actions Speak Louder Than Goals: Valuing Player Actions in Soccer](https://arxiv.org/abs/1802.07127) (VAEP, KDD 2019) | ценность действия как изменение P(забьём) − P(пропустим) |
| [Expected Threat (xT)](https://socceraction.readthedocs.io/en/stable/documentation/valuing_actions/xT.html) (socceraction) | xT Карана Сингха: формула и реализация |
| G. Krishnan. [Explaining Expected Threat (xT) Using Markov Models](https://gauravkrishnan.substack.com/p/explaining-expected-threat-xt-in-football-analytics-using-markov-models-its-history-part-i-20d4d31e2ea9) | xT как марковская цепь, история |
| [William Spearman: How Liverpool create pitch control models](https://archive.trainingground.guru/articles/william-spearman-how-liverpool-create-pitch-control) | pitch control на практике |
| [Soccermatics: Pass probability](https://soccermatics.readthedocs.io/en/latest/lesson6/PassProbability.html) | физическая модель вероятности паса |
| [Evaluation of creating scoring opportunities for teammates in soccer via trajectory prediction](https://arxiv.org/abs/2206.01899) | развитие OBSO |
| W. Spearman. Beyond Expected Goals (Sloan 2018), ссылку не нашёл | OBSO: ценность точки для игрока без мяча |
| T. Taki, J. Hasegawa. Visualization of dominant region in team games and its application to teamwork analysis (2000), ссылку не нашёл | «доминирующие области» — исходная идея границы «кто первым достигнет» |
| [Stats Perform: How we measure pressure](https://www.statsperform.com/resource/how-we-measure-pressure/) | давление на владельца |
| [Premier League: Passes per defensive action explained](https://www.premierleague.com/en/news/4250153) | PPDA |
