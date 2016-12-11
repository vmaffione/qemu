#!/bin/bash

WPS="221 226 231 236 241 247 252 258 264 270 276 282 288 295 301 308 315 322 329 337 344 352 360 368 377 385 394 403 412 421 430 440 450 460 471 481 492 503 514 526 538 550 562 575 588 601 615 628 643 657 672 687 703 718 734 751 768 785 803 821 839 858 878 897 918 938 959 981 1003 1026 1049 1072 1096 1121 1146 1172 1199 1225 1253 1281 1310 1340 1370 1401 1432 1464 1497 1531 1565 1601 1637 1673 1711 1750 1789 1829 1870 1912 1956"
WPS="$WPS 2000 2007 2015 2022 2030 2038 2046 2053 2061 2069 2077 2085 2093 2101 2109 2117 2125 2133 2141 2149 2157 2165 2174 2182 2190 2198 2207 2215 2224 2232 2241 2249 2258 2266 2275 2283 2292 2301 2310 2318 2327 2336 2345 2354 2363 2372 2381 2390 2399 2408 2417 2426 2436 2445 2454 2463 2473 2482 2492 2501 2511 2520 2530 2539 2549 2559 2568 2578 2588 2598 2608 2618 2628 2638 2648 2658 2668 2678 2688 2698 2708 2719 2729 2739 2750 2760 2771 2781 2792 2803 2813 2824 2835 2845 2856 2867 2878 2889 2900 2911"

./pc -H
for wp in $WPS; do
    ./pc -q -d 10 -c 2000 -p $wp
done